#include "vm.hpp"
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
#include <sstream>
#include <cctype>
#include <cstring>

namespace gm14 {

// ---------------- value helpers ----------------
double to_num(const Value& v) {
    if (v.type == Value::NUM) return v.num;
    if (v.type == Value::STR) { try { return std::stod(v.str); } catch (...) { return 0; } }
    return 0;
}
std::string to_str(const Value& v) {
    if (v.type == Value::STR) return v.str;
    if (v.type == Value::NUM) {
        double n = v.num;
        if (n == (long long)n && std::fabs(n) < 1e15) {
            char b[32]; std::snprintf(b, sizeof(b), "%lld", (long long)n); return b;
        }
        std::ostringstream ss; ss << n; return ss.str();
    }
    return "undefined";
}
bool to_bool(const Value& v) {
    if (v.type == Value::STR) return !v.str.empty();
    if (v.type == Value::NUM) return v.num != 0;
    return false;
}

static double fmod_pos(double a, double b) { if (b == 0) return 0; double r = std::fmod(a, b); return r; }

static int jump_offset(const Instruction& ins) {
    int v = ins.first_word & 0xFFFFFF;
    if (v != 0xF00000 && (v & 0x400000)) v |= 0x800000;
    if (v & 0x800000) v -= 0x1000000;
    return v;
}

// ---------------- construction ----------------
Runtime::Runtime(DataWin& dw_) : dw(dw_) {
    static const char* pre[] = {"gml_Script_", "gml_GlobalScript_"};
    for (auto& e : dw.code) {
        for (auto p : pre) {
            size_t n = std::strlen(p);
            if (e.name.compare(0, n, p) == 0) scripts[e.name.substr(n)] = &e;
        }
    }
    global_builtins["current_time"] = Value(0.0);
    global_builtins["delta_time"] = Value(16666.0);
    global_builtins["room_speed"] = Value(30.0);
    register_builtins();
}

// ---------------- audio helpers ----------------
void Runtime::ensure_audio() {
    if (!audio) {
        audio.reset(new AudioEngine());
#ifdef __3DS__
        // On hardware, route the mixer into the Teak DSP. Tests and the host
        // keep the deterministic null backend unless a backend is set explicitly.
        audio->set_backend(make_device_backend());
#endif
        // Lazy per-sound decoding with a bounded PCM budget. Decoding all 442
        // Undertale sounds eagerly needs tens of MB and exhausts the 3DS heap
        // (manifested as std::bad_alloc in step()). Clips are decoded on first
        // use and LRU-evicted when the budget is exceeded.
        DataWin* dwp = &dw;
        audio->set_clip_loader([dwp](int asset) -> std::shared_ptr<AudioClip> {
            if (asset < 0 || asset >= (int)dwp->sounds.size()) return nullptr;
            const Sound& s = dwp->sounds[asset];
            if (s.audo_id < 0 || s.audo_id >= (int)dwp->audio.size()) return nullptr;
            std::vector<uint8_t> bytes = dwp->audio_bytes(s.audo_id);
            if (bytes.empty()) return nullptr;
            std::shared_ptr<AudioClip> clip = std::make_shared<AudioClip>();
            if (!decode_audio(bytes.data(), bytes.size(), *clip)) return nullptr;
            return clip;
        }, 6u * 1024u * 1024u);  // ~6 MB PCM budget
    }
}

// A GML sound argument is either an asset index (0-based SOND index) or a
// play handle (>= kHandleBase). Return the asset index it refers to.
int Runtime::audio_asset_index(const Value& v) const {
    int id = (int)to_num(v);
    if (id >= kHandleBase) {
        // handled by the engine as a voice handle; caller decides.
        return id;
    }
    return id;
}

void Runtime::audio_register_clips() {
    // Lazy loading: the engine decodes clips on demand via the loader set in
    // ensure_audio(). This function only guarantees the engine exists.
    ensure_audio();
}

static bool is_obj(const DataWin& dw, Instance* inst, int target) {
    if (!inst) return false;
    int o = inst->obj;
    for (int i = 0; i < 16; ++i) {
        if (o == target) return true;
        if (o < 0 || o >= (int)dw.objects.size()) return false;
        o = dw.objects[o].parent;
    }
    return false;
}

Instance* Runtime::resolve_inst(int inst_type) {
    if (inst_type == -1) return cur;
    if (inst_type == -2) return other;
    if (inst_type >= 100000) {
        for (auto& i : instances) if (i->alive && i->iid == inst_type) return i.get();
    } else if (inst_type >= 0) {
        for (auto& i : instances) if (i->alive && is_obj(dw, i.get(), inst_type)) return i.get();
    }
    return cur;
}

// ---------------- variables ----------------
Value Runtime::get_var(Frame& fr, const Instruction& ins) {
    int inst_type = (int)(int16_t)ins.low16();
    if (inst_type == -9 || ins.ref_type() == 0x80) {
        if (!fr.stack.empty()) { inst_type = (int)to_num(fr.stack.back()); fr.stack.pop_back(); }
    }
    std::string name;
    if (ins.var >= 0 && ins.var < (int)dw.variables.size()) name = dw.variables[ins.var].name;
    int varid = (ins.var >= 0) ? dw.variables[ins.var].varid : 0;
    if (inst_type == -5) {
        auto it = globals.find(name); return it != globals.end() ? it->second : Value(0.0);
    }
    if (inst_type == -7) {
        return (varid >= 0 && varid < (int)fr.locals.size()) ? fr.locals[varid] : Value(0.0);
    }
    if (inst_type == -15) {
        return (varid >= 0 && varid < (int)fr.args.size()) ? fr.args[varid] : Value(0.0);
    }
    if (name == "argument_count") {
        return Value((double)fr.args.size());
    }
    // argument0..15 (varid -6, but only when the name is an argument)
    if (name.size() > 8 && name.compare(0, 8, "argument") == 0) {
        int idx = 0;
        std::string num = name.substr(8);
        if (!num.empty()) { try { idx = std::stoi(num); } catch (...) { idx = 0; } }
        return (idx >= 0 && idx < (int)fr.args.size()) ? fr.args[idx] : Value(0.0);
    }
    Instance* inst = resolve_inst(inst_type);
    if (inst) {
        auto it = inst->vars.find(name);
        if (it != inst->vars.end()) return it->second;
        // If this name has array contents (e.g. `charmap` built as an array),
        // expose them so string/draw builtins can index via the value.
        auto ai = inst->arrays.find(name);
        if (ai != inst->arrays.end() && !ai->second.empty()) {
            Value v; v.type = Value::VARPTR; v.arrref = &ai->second; return v;
        }
    }
    // builtin instance variables default
    static const std::unordered_map<std::string, double> defs = {
        {"x",0},{"y",0},{"hspeed",0},{"vspeed",0},{"direction",0},{"speed",0},
        {"image_index",0},{"image_speed",1},{"image_xscale",1},{"image_yscale",1},
        {"image_angle",0},{"image_alpha",1},{"image_blend",16777215},
        {"sprite_index",-1},{"mask_index",-1},{"depth",0},{"visible",1},{"solid",0},
        {"persistent",0},{"object_index",0},{"id",0},{"xstart",0},{"ystart",0},
        {"xprevious",0},{"yprevious",0},{"bbox_left",0},{"bbox_top",0},
        {"bbox_right",0},{"bbox_bottom",0},{"gravity",0},{"gravity_direction",270},
        {"friction",0},
    };
    auto d = defs.find(name);
    if (d != defs.end()) return Value(d->second);
    if (name == "room_speed" || name == "current_time" || name == "fps" ||
        name == "score" || name == "lives" || name == "health" || name == "os_type")
        return global_builtins.count(name) ? global_builtins[name] : Value(0.0);
    if (name == "view_current") return globals.count("view_current") ? globals["view_current"] : Value(0.0);
    if (name == "view_enabled") return globals.count("view_enabled") ? globals["view_enabled"] : Value(1.0);
    if (name == "room_width") return globals.count("room_width") ? globals["room_width"] : Value(320.0);
    if (name == "room_height") return globals.count("room_height") ? globals["room_height"] : Value(240.0);
    if (name == "application_surface") return Value(0.0);
    return Value(0.0);
}

void Runtime::set_var(Frame& fr, const Instruction& ins, const Value& v) {
    int inst_type = (int)(int16_t)ins.low16();
    if (inst_type == -9 || ins.ref_type() == 0x80) {
        if (!fr.stack.empty()) { inst_type = (int)to_num(fr.stack.back()); fr.stack.pop_back(); }
    }
    std::string name;
    if (ins.var >= 0 && ins.var < (int)dw.variables.size()) name = dw.variables[ins.var].name;
    int varid = (ins.var >= 0) ? dw.variables[ins.var].varid : 0;
    if (name == "view_current" || name == "view_enabled" || name == "room_width" || name == "room_height") {
        globals[name] = v;
        return;
    }
    if (inst_type == -5) { globals[name] = v; return; }
    if (inst_type == -7) { if (varid >= 0 && varid < (int)fr.locals.size()) fr.locals[varid] = v; return; }
    if (inst_type == -15) { if (varid >= 0 && varid < (int)fr.args.size()) fr.args[varid] = v; return; }
    if (name.size() > 8 && name.compare(0, 8, "argument") == 0) {
        int idx = 0; std::string num = name.substr(8);
        if (!num.empty()) { try { idx = std::stoi(num); } catch (...) {} }
        if (idx >= 0 && idx < (int)fr.args.size()) fr.args[idx] = v;
        return;
    }
    Instance* inst = resolve_inst(inst_type);
    if (inst) inst->vars[name] = v;
}

Value Runtime::get_array(Frame& fr, const Instruction& ins, int inst_type, int index) {
    if (inst_type == -9 || ins.ref_type() == 0x80) {
        if (!fr.stack.empty()) { inst_type = (int)to_num(fr.stack.back()); fr.stack.pop_back(); }
    }
    std::string name;
    if (ins.var >= 0 && ins.var < (int)dw.variables.size()) name = dw.variables[ins.var].name;
    if (name.compare(0, 5, "view_") == 0) {
        auto it = global_arrays.find(name);
        if (it != global_arrays.end() && index >= 0 && index < (int)it->second.size()) return it->second[index];
        if (name == "view_wview") return Value(320.0);
        if (name == "view_hview") return Value(240.0);
        return Value(0.0);
    }
    if (name == "argument") {
        if (index >= 0 && index < (int)fr.args.size()) return fr.args[index];
        return Value(0.0);
    }
    if (name == "alarm") {
        Instance* inst = (inst_type > 0) ? resolve_inst(inst_type) : cur;
        if (inst && index >= 0 && index < 12) return Value(inst->alarms[index] < 0 ? -1.0 : (double)inst->alarms[index]);
        return Value(-1.0);
    }
    if (inst_type == -5) {
        auto it = global_arrays.find(name);
        if (it != global_arrays.end() && index >= 0 && index < (int)it->second.size()) return it->second[index];
        return Value(0.0);
    }
    if (inst_type == -7) {
        int varid = (ins.var >= 0) ? dw.variables[ins.var].varid : 0;
        auto it = fr.local_arrays.find(varid);
        if (it != fr.local_arrays.end() && index >= 0 && index < (int)it->second.size()) return it->second[index];
        return Value(0.0);
    }
    Instance* inst = resolve_inst(inst_type);
    if (inst) {
        auto it = inst->arrays.find(name);
        if (it != inst->arrays.end() && index >= 0 && index < (int)it->second.size()) return it->second[index];
    }
    return Value(0.0);
}

void Runtime::set_array(Frame& fr, const Instruction& ins, int inst_type, int index, const Value& v) {
    if (inst_type == -9 || ins.ref_type() == 0x80) {
        if (!fr.stack.empty()) { inst_type = (int)to_num(fr.stack.back()); fr.stack.pop_back(); }
    }
    std::string name;
    if (ins.var >= 0 && ins.var < (int)dw.variables.size()) name = dw.variables[ins.var].name;
    if (name.compare(0, 5, "view_") == 0) {
        auto& arr = global_arrays[name];
        if (index >= 0) {
            if (index >= (int)arr.size()) arr.resize(index + 1, Value(0.0));
            arr[index] = v;
        }
        return;
    }
    if (name == "argument") {
        if (index >= 0) {
            if (index >= (int)fr.args.size()) fr.args.resize(index + 1, Value(0.0));
            fr.args[index] = v;
        }
        return;
    }
    if (name == "alarm") {
        Instance* inst = (inst_type > 0) ? resolve_inst(inst_type) : cur;
        if (inst && index >= 0 && index < 12) inst->alarms[index] = (to_num(v) < 0) ? -1 : (int)to_num(v);
        return;
    }
    std::vector<Value>* arr = nullptr;
    if (inst_type == -5) arr = &global_arrays[name];
    else if (inst_type == -7) {
        int varid = (ins.var >= 0) ? dw.variables[ins.var].varid : 0;
        arr = &fr.local_arrays[varid];
    } else {
        Instance* inst = resolve_inst(inst_type);
        if (!inst) return;
        arr = &inst->arrays[name];
    }
    if (index < 0) return;
    if (index >= (int)arr->size()) arr->resize(index + 1, Value(0.0));
    (*arr)[index] = v;
}

// ---------------- execution ----------------
Value Runtime::call(const std::string& name, std::vector<Value>& args) {
    if (diag_sink) {
        std::string line = "call " + name + " argc=" + std::to_string(args.size());
        for (size_t i = 0; i < args.size() && i < 6; ++i) {
            line += " ";
            if (args[i].is_str()) line += "\"" + args[i].str + "\"";
            else line += std::to_string((long long)args[i].num);
        }
        diag_sink(line.c_str());
    }
    // GMS resolution order: a user script with the same name as a builtin
    // SHADOWS the builtin. This matters for Undertale, which defines its own
    // control_* / draw_* etc. scripts. Check scripts first.
    auto s = scripts.find(name);
    if (s != scripts.end()) return run_entry(s->second, args, cur, other);
    auto b = builtins.find(name);
    if (b != builtins.end()) return b->second(*this, args);
    // Unknown function: log once, record it, pop nothing extra (args are already
    // consumed by the caller), and push a default real 0.0 so the game keeps
    // running instead of crashing.
    if (warned.insert(name).second) {
        std::printf("[VM UNIMPLEMENTED] Function: %s (argc=%d) | IP: 0x%08X\n",
                    name.c_str(), (int)args.size(), (unsigned)frame_count);
        std::fflush(stdout);
        m_unimplemented.push_back(name);
    }
    if (diag_sink) { std::string l = "UNIMPLEMENTED " + name; diag_sink(l.c_str()); }
    return Value(0.0);
}

Value Runtime::run_entry(const CodeEntry* entry, std::vector<Value>& args, Instance* self, Instance* other_) {
    Frame fr;
    fr.entry = entry;
    fr.base = dw.all_instrs.data() + entry->instr_start;
    fr.n = (int)entry->instr_count;
    uint32_t a = 0;
    for (int i = 0; i < fr.n; ++i) {
        fr.addrs.push_back(a);
        fr.addr_index[a] = i;
        a += fr.base[i].size();
    }
    fr.args = args;
    fr.locals.assign(std::max(1, (int)entry->locals), Value(0.0));
    if (!fr.locals.empty()) {
        std::vector<Value> a0 = args; // "arguments" array slot 0
        fr.local_arrays[0] = a0;
    }
    fr.self = self; fr.other = other_;
    Instance* oc = cur; Instance* oo = other;
    cur = self; other = other_;
    try { exec(fr); }
    catch (...) { cur = oc; other = oo; throw; }
    cur = oc; other = oo;
    return fr.value;
}

void Runtime::exec(Frame& fr) {
    const Instruction* ins_list = fr.base;
    int n = fr.n;
    long long budget = 20000000;
    long long steps = 0;
    while (fr.pc < n) {
        if (++steps > budget) throw std::runtime_error("instruction budget exceeded in " + fr.entry->name);
        const Instruction& ins = ins_list[fr.pc];
        uint8_t k = ins.kind(), t1 = ins.type1();
        // Safe stack pop: GML bytecode occasionally under-flows on unusual paths
        // (malformed branch targets, missing event setup). Popping an empty stack
        // is undefined behaviour and manifests as a segfault or bad_alloc, so we
        // substitute 0.0 instead of crashing. See HANDOFF.md.
        auto stk_pop = [&]() -> Value {
            if (fr.stack.empty()) { if (diag_sink) diag_sink("STACK_UNDERFLOW"); return Value(0.0); }
            Value v = fr.stack.back(); fr.stack.pop_back(); return v;
        };
        auto is_push = [](uint8_t kk){ return kk==OP_PUSH||kk==OP_PUSHLOC||kk==OP_PUSHGLB||kk==OP_PUSHBLTN||kk==OP_PUSHI; };
        if (is_push(k)) {
            if (t1 == 0x0F) fr.stack.push_back(Value(ins.value));
            else if (t1 == 6) { uint32_t idx = ins.str_index(); fr.stack.push_back(Value(idx < dw.strings.size() ? dw.strings[idx] : std::string())); }
            else if (ins.var >= 0) {
                if (ins.ref_type() == 0x00) {
                    int idx = (int)to_num(stk_pop());
                    int it = (int)to_num(stk_pop());
                    fr.stack.push_back(get_array(fr, ins, it, idx));
                } else fr.stack.push_back(get_var(fr, ins));
            } else if (ins.fun >= 0) fr.stack.push_back(Value(dw.functions[ins.fun].name));
            else fr.stack.push_back(Value(ins.value));
            fr.pc++;
        } else if (k == OP_POP) {
            if (t1 == 0x0F) {
                if (fr.stack.size() >= 2) std::swap(fr.stack[fr.stack.size()-1], fr.stack[fr.stack.size()-2]);
            } else if (ins.var >= 0 && ins.ref_type() == 0x00) {
                int idx = (int)to_num(stk_pop());
                int it = (int)to_num(stk_pop());
                Value v = stk_pop();
                set_array(fr, ins, it, idx, v);
            } else {
                Value v = stk_pop();
                set_var(fr, ins, v);
            }
            fr.pc++;
        } else if (k == OP_POPZ) {
            if (!fr.stack.empty()) fr.stack.pop_back(); fr.pc++;
        } else if (k == OP_DUP) {
            if (!fr.stack.empty()) fr.stack.push_back(fr.stack.back()); fr.pc++;
        } else if (k == OP_CONV) {
            Value v = stk_pop();
            uint8_t dst = ins.type2();
            if (dst == 5) { /* GML variant: preserve value type */ }
            else if (dst == 2 || dst == 3 || dst == 0x0F) v = Value((double)(long long)to_num(v));
            else if (dst == 4) v = Value(to_bool(v) ? 1.0 : 0.0);
            else if (dst == 6) v = Value(to_str(v));
            else v = Value(to_num(v));
            fr.stack.push_back(v); fr.pc++;
        } else if (k == OP_ADD) {
            Value b = stk_pop();
            Value a = stk_pop();
            if (a.is_str() || b.is_str()) fr.stack.push_back(Value(to_str(a) + to_str(b)));
            else fr.stack.push_back(Value(to_num(a) + to_num(b)));
            fr.pc++;
        } else if (k == OP_SUB || k == OP_MUL || k == OP_DIV || k == OP_REM || k == OP_MOD ||
                   k == OP_AND || k == OP_OR || k == OP_XOR || k == OP_SHL || k == OP_SHR) {
            Value b = stk_pop();
            Value a = stk_pop();
            double x = to_num(a), y = to_num(b), r = 0;
            switch (k) {
                case OP_SUB: r = x - y; break;
                case OP_MUL: r = x * y; break;
                case OP_DIV: r = y ? x / y : 0; break;
                case OP_REM: case OP_MOD: r = fmod_pos(x, y); break;
                case OP_AND: r = (double)((long long)x & (long long)y); break;
                case OP_OR:  r = (double)((long long)x | (long long)y); break;
                case OP_XOR: r = (double)((long long)x ^ (long long)y); break;
                case OP_SHL: r = (double)((long long)x << (long long)y); break;
                case OP_SHR: r = (double)((long long)x >> (long long)y); break;
            }
            fr.stack.push_back(Value(r)); fr.pc++;
        } else if (k == OP_NEG) {
            Value v = stk_pop(); fr.stack.push_back(Value(-to_num(v))); fr.pc++;
        } else if (k == OP_NOT) {
            Value v = stk_pop();
            if (t1 == 4) fr.stack.push_back(Value(!to_bool(v) ? 1.0 : 0.0));
            else fr.stack.push_back(Value((double)(~(long long)to_num(v))));
            fr.pc++;
        } else if (k == OP_CMP) {
            Value b = stk_pop();
            Value a = stk_pop();
            bool r;
            if (a.is_str() || b.is_str()) {
                int c = to_str(a).compare(to_str(b));
                switch (ins.cmp()) { case 1: r=c<0; break; case 2: r=c<=0; break; case 3: r=c==0; break;
                    case 4: r=c!=0; break; case 5: r=c>=0; break; case 6: r=c>0; break; default: r=false; }
            } else {
                double x=to_num(a), y=to_num(b);
                switch (ins.cmp()) { case 1: r=x<y; break; case 2: r=x<=y; break; case 3: r=x==y; break;
                    case 4: r=x!=y; break; case 5: r=x>=y; break; case 6: r=x>y; break; default: r=false; }
            }
            fr.stack.push_back(Value(r ? 1.0 : 0.0)); fr.pc++;
        } else if (k == OP_B || k == OP_BT || k == OP_BF) {
            bool take = true;
            Value bval = fr.stack.empty() ? Value(0.0) : fr.stack.back();
            if (k != OP_B) { bool c = to_bool(stk_pop()); take = (k == OP_BT) ? c : !c; }
            if (diag_sink && fr.entry && fr.entry->name=="gml_Script_scr_namingscreen" && fr.pc>=378 && fr.pc<=420) {
                std::string l="BR pc="+std::to_string(fr.pc)+" k=0x"+std::to_string((int)k)+" top="+std::to_string((long long)to_num(bval))+" take="+std::to_string((int)take)+" stack="+std::to_string(fr.stack.size());
                diag_sink(l.c_str());
            }
            if (take) {
                int target = (int)(fr.addrs[fr.pc] + jump_offset(ins));
                auto it = fr.addr_index.find(target);
                fr.pc = (it != fr.addr_index.end()) ? it->second : n;
            } else fr.pc++;
        } else if (k == OP_PUSHENV) {
            Value v = stk_pop();
            int target_id = (int)to_num(v);
            std::vector<Instance*> targets;
            if (target_id == -1) {
                if (cur && cur->alive) targets.push_back(cur);
            } else if (target_id == -2) {
                if (other && other->alive) targets.push_back(other);
            } else if (target_id == -3) {
                for (auto& i : instances) if (i->alive) targets.push_back(i.get());
            } else if (target_id >= 100000) {
                for (auto& i : instances) if (i->alive && i->iid == target_id) { targets.push_back(i.get()); break; }
            } else if (target_id >= 0) {
                for (auto& i : instances) if (i->alive && is_obj(dw, i.get(), target_id)) targets.push_back(i.get());
            }
            if (targets.empty()) {
                int target_addr = (int)(fr.addrs[fr.pc] + jump_offset(ins));
                auto it = fr.addr_index.find(target_addr);
                fr.pc = (it != fr.addr_index.end()) ? it->second : n;
            } else {
                EnvFrame ef;
                ef.saved_cur = cur;
                ef.saved_other = other;
                ef.inst_list = std::move(targets);
                ef.inst_idx = 0;
                ef.loop_pc = fr.pc + 1;
                other = cur;
                cur = ef.inst_list[0];
                env_stack.push_back(std::move(ef));
                fr.pc++;
            }
        } else if (k == OP_POPENV) {
            if (!env_stack.empty()) {
                auto& ef = env_stack.back();
                ef.inst_idx++;
                while (ef.inst_idx < ef.inst_list.size() && !ef.inst_list[ef.inst_idx]->alive) {
                    ef.inst_idx++;
                }
                if (ef.inst_idx < ef.inst_list.size()) {
                    cur = ef.inst_list[ef.inst_idx];
                    fr.pc = ef.loop_pc;
                } else {
                    cur = ef.saved_cur;
                    other = ef.saved_other;
                    env_stack.pop_back();
                    fr.pc++;
                }
            } else {
                fr.pc++;
            }
        } else if (k == OP_CALL) {
            int argc = ins.low16();
            std::vector<Value> args(argc);
            for (int i = 0; i < argc; ++i) { args[i] = stk_pop(); }
            std::string name = (ins.fun >= 0 && ins.fun < (int)dw.functions.size()) ? dw.functions[ins.fun].name : "";
            fr.stack.push_back(call(name, args));
            fr.pc++;
        } else if (k == OP_RET) {
            fr.value = fr.stack.empty() ? Value(0.0) : fr.stack.back();
            while (!env_stack.empty()) { cur = env_stack.back().saved_cur; other = env_stack.back().saved_other; env_stack.pop_back(); }
            return;
        } else if (k == OP_EXIT) {
            while (!env_stack.empty()) { cur = env_stack.back().saved_cur; other = env_stack.back().saved_other; env_stack.pop_back(); }
            return;
        } else if (k == OP_CALLV) {
            int argc = ins.low16();
            std::vector<Value> args(argc);
            for (int i = 0; i < argc; ++i) { args[i] = stk_pop(); }
            Value fn = stk_pop();
            fr.stack.push_back(fn.is_str() ? call(fn.str, args) : Value(0.0));
            fr.pc++;
        } else {
            fr.pc++;
        }
    }
}

// ---------------- images / drawing ----------------
const Image& Runtime::page(int tex_index) {
    auto it = page_cache.find(tex_index);
    if (it == page_cache.end()) it = page_cache.emplace(tex_index, dw.load_texture(tex_index)).first;
    return it->second;
}

static Image crop_img(const Image& src, int x0, int y0, int w, int h) {
    Image out; out.w = w; out.h = h; out.rgba.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        int sx = x0 + x, sy = y0 + y;
        if (sx < 0 || sy < 0 || sx >= src.w || sy >= src.h) continue;
        std::memcpy(&out.rgba[((size_t)y * w + x) * 4], &src.rgba[((size_t)sy * src.w + sx) * 4], 4);
    }
    return out;
}

bool Runtime::tpag_image(int tpag_index, Image& out, int& tx, int& ty) {
    if (tpag_index < 0 || tpag_index >= (int)dw.tpags.size()) return false;
    const Tpag& t = dw.tpags[tpag_index];
    if (t.tex < 0 || t.tex >= (int)dw.tex_ptrs.size()) return false;
    const Image& pg = page(t.tex);
    if (pg.w <= 0) return false;
    if (t.sx < 0 || t.sy < 0 || t.sx + t.sw > pg.w || t.sy + t.sh > pg.h) return false;
    out = crop_img(pg, t.sx, t.sy, t.sw, t.sh);
    tx = t.tx; ty = t.ty;
    return true;
}

void Runtime::blit_screen(const Image& src, int dx, int dy, double alpha) {
    if (!screen || src.w <= 0 || src.h <= 0) return;
    dx -= view_x;
    dy -= view_y;
    int a = (int)(std::max(0.0, std::min(1.0, alpha)) * 255);
    for (int y = 0; y < src.h; ++y) {
        int ty = dy + y; if (ty < 0 || ty >= screen->h) continue;
        for (int x = 0; x < src.w; ++x) {
            int tx = dx + x; if (tx < 0 || tx >= screen->w) continue;
            const uint8_t* s = &src.rgba[((size_t)y * src.w + x) * 4];
            uint8_t* d = &screen->rgba[((size_t)ty * screen->w + tx) * 4];
            double sa = (s[3] / 255.0) * (a / 255.0);
            if (sa <= 0) continue;
            for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::min(255.0, s[c] * sa + d[c] * (1 - sa));
        }
    }
}

void Runtime::blit_sub_screen(const Image& src, int sx, int sy, int sw, int sh, int dx, int dy, double alpha) {
    if (!screen || sw <= 0 || sh <= 0 || src.w <= 0 || src.h <= 0) return;
    int a = (int)(std::max(0.0, std::min(1.0, alpha)) * 255);
    if (a <= 0) return;
    for (int y = 0; y < sh; ++y) {
        int ty = dy + y; if (ty < 0 || ty >= screen->h) continue;
        int sy_pos = sy + y; if (sy_pos < 0 || sy_pos >= src.h) continue;
        for (int x = 0; x < sw; ++x) {
            int tx = dx + x; if (tx < 0 || tx >= screen->w) continue;
            int sx_pos = sx + x; if (sx_pos < 0 || sx_pos >= src.w) continue;
            const uint8_t* s = &src.rgba[((size_t)sy_pos * src.w + sx_pos) * 4];
            uint8_t* d = &screen->rgba[((size_t)ty * screen->w + tx) * 4];
            if (s[3] == 0) continue;
            if (s[3] == 255 && a == 255) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            } else {
                double sa = (s[3] / 255.0) * (a / 255.0);
                for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::min(255.0, s[c] * sa + d[c] * (1.0 - sa));
                d[3] = 255;
            }
        }
    }
}

void Runtime::blit_sub_screen_scaled(const Image& src, int sx, int sy, int sw, int sh,
                                     int dx, int dy, int out_w, int out_h,
                                     double alpha, uint32_t color, double angle_deg) {
    if (!screen || sw <= 0 || sh <= 0 || src.w <= 0 || src.h <= 0) return;
    if (out_w <= 0 || out_h <= 0) return;
    int a255 = (int)(std::max(0.0, std::min(1.0, alpha)) * 255);
    if (a255 <= 0) return;
    uint8_t tr = (uint8_t)(color & 0xFF);
    uint8_t tg = (uint8_t)((color >> 8) & 0xFF);
    uint8_t tb = (uint8_t)((color >> 16) & 0xFF);
    double ang = angle_deg * 3.14159265358979 / 180.0;
    double ca = std::cos(ang), sa = std::sin(ang);
    // Centre of the destination rect (rotation pivot).
    double cx = dx + out_w * 0.5;
    double cy = dy + out_h * 0.5;
    // Bounding box of the rotated rect so we know which dest pixels to visit.
    double hw = out_w * 0.5, hh = out_h * 0.5;
    int bb_w = (int)std::ceil(std::abs(out_w * ca) + std::abs(out_h * sa)) + 2;
    int bb_h = (int)std::ceil(std::abs(out_w * sa) + std::abs(out_h * ca)) + 2;
    int bb_x = (int)(cx - bb_w * 0.5);
    int bb_y = (int)(cy - bb_h * 0.5);
    for (int py = 0; py < bb_h; ++py) {
        int ty = bb_y + py; if (ty < 0 || ty >= screen->h) continue;
        for (int px = 0; px < bb_w; ++px) {
            int tx = bb_x + px; if (tx < 0 || tx >= screen->w) continue;
            // Inverse-rotate the dest point into the unrotated dest rect space.
            double rx = (tx + 0.5) - cx;
            double ry = (ty + 0.5) - cy;
            double ux = rx * ca + ry * sa;   // -hw..hw
            double uy = -rx * sa + ry * ca;  // -hh..hh
            if (ux < -hw || ux >= hw || uy < -hh || uy >= hh) continue;
            // Map into source sub-rect.
            double fx = (ux + hw) / (double)out_w;   // 0..1
            double fy = (uy + hh) / (double)out_h;   // 0..1
            int ssx = sx + (int)(fx * sw);
            int ssy = sy + (int)(fy * sh);
            if (ssx < 0 || ssx >= src.w || ssy < 0 || ssy >= src.h) continue;
            const uint8_t* s = &src.rgba[((size_t)ssy * src.w + ssx) * 4];
            if (s[3] == 0) continue;
            uint8_t* d = &screen->rgba[((size_t)ty * screen->w + tx) * 4];
            uint8_t sr = (uint8_t)((s[0] * tr) / 255);
            uint8_t sg = (uint8_t)((s[1] * tg) / 255);
            uint8_t sb = (uint8_t)((s[2] * tb) / 255);
            if (s[3] == 255 && a255 == 255) {
                d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255;
            } else {
                double sfa = (s[3] / 255.0) * (a255 / 255.0);
                d[0] = (uint8_t)std::min(255.0, sr * sfa + d[0] * (1.0 - sfa));
                d[1] = (uint8_t)std::min(255.0, sg * sfa + d[1] * (1.0 - sfa));
                d[2] = (uint8_t)std::min(255.0, sb * sfa + d[2] * (1.0 - sfa));
                d[3] = 255;
            }
        }
    }
}

void Runtime::blit_sub_screen_tint(const Image& src, int sx, int sy, int sw, int sh,
                                  int dx, int dy, double alpha, uint32_t color) {
    if (!screen || sw <= 0 || sh <= 0 || src.w <= 0 || src.h <= 0) return;
    int a = (int)(std::max(0.0, std::min(1.0, alpha)) * 255);
    if (a <= 0) return;
    uint8_t tr = (uint8_t)(color & 0xFF);
    uint8_t tg = (uint8_t)((color >> 8) & 0xFF);
    uint8_t tb = (uint8_t)((color >> 16) & 0xFF);

    for (int y = 0; y < sh; ++y) {
        int ty = dy + y; if (ty < 0 || ty >= screen->h) continue;
        int sy_pos = sy + y; if (sy_pos < 0 || sy_pos >= src.h) continue;
        for (int x = 0; x < sw; ++x) {
            int tx = dx + x; if (tx < 0 || tx >= screen->w) continue;
            int sx_pos = sx + x; if (sx_pos < 0 || sx_pos >= src.w) continue;
            const uint8_t* s = &src.rgba[((size_t)sy_pos * src.w + sx_pos) * 4];
            if (s[3] == 0) continue;
            uint8_t* d = &screen->rgba[((size_t)ty * screen->w + tx) * 4];

            uint8_t sr = (uint8_t)((s[0] * tr) / 255);
            uint8_t sg = (uint8_t)((s[1] * tg) / 255);
            uint8_t sb = (uint8_t)((s[2] * tb) / 255);

            if (s[3] == 255 && a == 255) {
                d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255;
            } else {
                double sa = (s[3] / 255.0) * (a / 255.0);
                d[0] = (uint8_t)std::min(255.0, sr * sa + d[0] * (1.0 - sa));
                d[1] = (uint8_t)std::min(255.0, sg * sa + d[1] * (1.0 - sa));
                d[2] = (uint8_t)std::min(255.0, sb * sa + d[2] * (1.0 - sa));
                d[3] = 255;
            }
        }
    }
}

static void draw_sprite_impl(Runtime& rt, double sprite, double subimg, double x, double y,
                             double xs, double ys, double alpha) {
    int si = (int)sprite;
    if (si < 0 || si >= (int)rt.dw.sprites.size()) return;
    const Sprite& sp = rt.dw.sprites[si];
    if (sp.frames.empty()) return;
    int n = (int)sp.frames.size();
    int fi = ((int)subimg % n + n) % n;
    int tpag_idx = sp.frames[fi];
    if (tpag_idx < 0 || tpag_idx >= (int)rt.dw.tpags.size()) return;
    const Tpag& t = rt.dw.tpags[tpag_idx];
    if (t.tex < 0 || t.tex >= (int)rt.dw.tex_ptrs.size()) return;
    const Image& pg = rt.page(t.tex);
    if (pg.w <= 0 || t.sx < 0 || t.sy < 0 || t.sx + t.sw > pg.w || t.sy + t.sh > pg.h) return;
    // Destination top-left in screen space (origin-relative).
    int dx = (int)std::lround(x - sp.origin_x * xs) + t.tx - rt.view_x;
    int dy = (int)std::lround(y - sp.origin_y * ys) + t.ty - rt.view_y;
    int out_w = (int)std::lround(t.sw * xs);
    int out_h = (int)std::lround(t.sh * ys);
    if (out_w == t.sw && out_h == t.sh) {
        rt.blit_sub_screen(pg, t.sx, t.sy, t.sw, t.sh, dx, dy, alpha);
    } else {
        rt.blit_sub_screen_scaled(pg, t.sx, t.sy, t.sw, t.sh, dx, dy, out_w, out_h, alpha, 0xFFFFFF, 0.0);
    }
}

// Draw a sprite stretched to exactly (w,h) with top-left at (x,y).
static void draw_sprite_stretched_impl(Runtime& rt, double sprite, double subimg,
                                       double x, double y, double w, double h, double alpha) {
    int si = (int)sprite;
    if (si < 0 || si >= (int)rt.dw.sprites.size()) return;
    const Sprite& sp = rt.dw.sprites[si];
    if (sp.frames.empty()) return;
    int n = (int)sp.frames.size();
    int fi = ((int)subimg % n + n) % n;
    int tpag_idx = sp.frames[fi];
    if (tpag_idx < 0 || tpag_idx >= (int)rt.dw.tpags.size()) return;
    const Tpag& t = rt.dw.tpags[tpag_idx];
    if (t.tex < 0 || t.tex >= (int)rt.dw.tex_ptrs.size()) return;
    const Image& pg = rt.page(t.tex);
    if (pg.w <= 0) return;
    rt.blit_sub_screen_scaled(pg, t.sx, t.sy, t.sw, t.sh,
                              (int)std::lround(x) - rt.view_x, (int)std::lround(y) - rt.view_y,
                              (int)std::lround(w), (int)std::lround(h), alpha, 0xFFFFFF, 0.0);
}

// ---------------- builtins ----------------
static Value vnum(double d) { return Value(d); }
static std::string sarg(std::vector<Value>& a, size_t i) { return i < a.size() ? to_str(a[i]) : std::string(); }
static double narg(std::vector<Value>& a, size_t i) { return i < a.size() ? to_num(a[i]) : 0.0; }


// ---------------- collision helpers ----------------
static bool bbox_of(Runtime& rt, Instance* inst, double x, double y, double* bb) {
    auto& v = inst->vars;
    int sprite = v.count("mask_index") ? (int)to_num(v["mask_index"]) : -1;
    if (sprite < 0) sprite = v.count("sprite_index") ? (int)to_num(v["sprite_index"]) : -1;
    if (sprite < 0 || sprite >= (int)rt.dw.sprites.size()) return false;
    const Sprite& s = rt.dw.sprites[sprite];
    double xs = v.count("image_xscale") ? to_num(v["image_xscale"]) : 1.0;
    double ys = v.count("image_yscale") ? to_num(v["image_yscale"]) : 1.0;
    double l = s.ml, r = s.mr, t = s.mt, b = s.mb;
    if (r <= l) r = (double)std::max<uint32_t>(1, s.width) - 1;
    if (b <= t) b = (double)std::max<uint32_t>(1, s.height) - 1;
    double x0 = (l - s.origin_x) * xs;
    double x1 = (r - s.origin_x) * xs;
    double y0 = (t - s.origin_y) * ys;
    double y1 = (b - s.origin_y) * ys;
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    bb[0] = x + x0; bb[1] = y + y0;
    bb[2] = x + x1; bb[3] = y + y1;
    return true;
}
static bool overlap(const double* a, const double* b) {
    return !(a[2] < b[0] || a[0] > b[2] || a[3] < b[1] || a[1] > b[3]);
}
static double collision_rect(Runtime& rt, double x1, double y1, double x2, double y2, int target, bool notme) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    double rect[4] = {x1, y1, x2, y2};
    for (auto& ip : rt.instances) {
        if (!ip->alive) continue;
        if (notme && rt.cur && ip->iid == rt.cur->iid) continue;
        if (!is_obj(rt.dw, ip.get(), target)) continue;
        double bb[4];
        if (bbox_of(rt, ip.get(), to_num(ip->vars.count("x")?ip->vars["x"]:Value(0.0)),
                    to_num(ip->vars.count("y")?ip->vars["y"]:Value(0.0)), bb) && overlap(rect, bb))
            return (double)ip->iid;
    }
    return -4.0;
}
static double instance_place_impl(Runtime& rt, double x, double y, int target) {
    if (!rt.cur) return -4.0;
    double me[4];
    if (!bbox_of(rt, rt.cur, x, y, me)) return -4.0;
    for (auto& ip : rt.instances) {
        if (!ip->alive || ip->iid == rt.cur->iid) continue;
        if (!is_obj(rt.dw, ip.get(), target)) continue;
        double bb[4];
        if (bbox_of(rt, ip.get(), to_num(ip->vars.count("x")?ip->vars["x"]:Value(0.0)),
                    to_num(ip->vars.count("y")?ip->vars["y"]:Value(0.0)), bb) && overlap(me, bb))
            return (double)ip->iid;
    }
    return -4.0;
}

void Runtime::register_builtins() {
    builtins["draw_sprite"] = [](Runtime& rt, std::vector<Value>& a){
        draw_sprite_impl(rt, narg(a,0), narg(a,1), narg(a,2), narg(a,3), 1, 1, 1); return vnum(0); };
    builtins["draw_sprite_ext"] = [](Runtime& rt, std::vector<Value>& a){
        draw_sprite_impl(rt, narg(a,0), narg(a,1), narg(a,2), narg(a,3), narg(a,4), narg(a,5),
                         a.size()>7?narg(a,7):1); return vnum(0); };
    builtins["draw_sprite_stretched"] = [](Runtime& rt, std::vector<Value>& a){
        draw_sprite_stretched_impl(rt, narg(a,0), narg(a,1), narg(a,2), narg(a,3), narg(a,4), narg(a,5), 1.0);
        return vnum(0); };
    builtins["draw_sprite_stretched_ext"] = [](Runtime& rt, std::vector<Value>& a){
        draw_sprite_stretched_impl(rt, narg(a,0), narg(a,1), narg(a,2), narg(a,3), narg(a,4), narg(a,5),
                                   a.size()>7?narg(a,7):1.0);
        return vnum(0); };
    builtins["draw_self"] = [](Runtime& rt, std::vector<Value>&){
        if (!rt.cur) return vnum(0);
        auto& v = rt.cur->vars;
        draw_sprite_impl(rt, v.count("sprite_index")?to_num(v["sprite_index"]):-1,
                         v.count("image_index")?to_num(v["image_index"]):0,
                         v.count("x")?to_num(v["x"]):0, v.count("y")?to_num(v["y"]):0,
                         v.count("image_xscale")?to_num(v["image_xscale"]):1,
                         v.count("image_yscale")?to_num(v["image_yscale"]):1,
                         v.count("image_alpha")?to_num(v["image_alpha"]):1);
        return vnum(0); };
    builtins["draw_set_color"] = [](Runtime& rt, std::vector<Value>& a){ rt.draw_color=(int)narg(a,0); return vnum(0); };
    builtins["draw_set_colour"] = builtins["draw_set_color"];
    builtins["draw_set_alpha"] = [](Runtime& rt, std::vector<Value>& a){ rt.draw_alpha=narg(a,0); return vnum(0); };
    // Render text with optional scale/rotation. Arg layout follows GML:
    //   draw_text(x, y, str)
    //   draw_text_transformed(x, y, str, xscale, yscale, angle)
    //   draw_text_ext(x, y, str, sep, w)
    //   ..._color variants append c1..c4(, alpha...)
    // We look up optional scale/angle by fixed indices passed in.
    auto render_text_fn = [](Runtime& rt, std::vector<Value>& a,
                             int color_arg_idx, int alpha_arg_idx,
                             int xscale_idx, int yscale_idx, int angle_idx) {
        if (a.size() < 3) return vnum(0);
        std::string txt = to_str(a[2]);
        if (txt.empty() || txt == "undefined") return vnum(0);
        double x0 = to_num(a[0]), y0 = to_num(a[1]);
        uint32_t col = (color_arg_idx >= 0 && (int)a.size() > color_arg_idx) ? (uint32_t)to_num(a[color_arg_idx]) : rt.draw_color;
        double alp = (alpha_arg_idx >= 0 && (int)a.size() > alpha_arg_idx) ? to_num(a[alpha_arg_idx]) : rt.draw_alpha;
        double xscale = (xscale_idx >= 0 && (int)a.size() > xscale_idx) ? to_num(a[xscale_idx]) : 1.0;
        double yscale = (yscale_idx >= 0 && (int)a.size() > yscale_idx) ? to_num(a[yscale_idx]) : 1.0;
        double angle  = (angle_idx  >= 0 && (int)a.size() > angle_idx)  ? to_num(a[angle_idx])  : 0.0;
        if (xscale == 0.0) xscale = 1.0;
        if (yscale == 0.0) yscale = 1.0;

        int fi = rt.draw_font;
        if (fi < 0 && rt.dw.fonts.size() > 1) fi = 1;
        else if (fi < 0 && !rt.dw.fonts.empty()) fi = 0;
        if (fi >= 0 && fi < (int)rt.dw.fonts.size() && !rt.dw.fonts[fi].glyphs.empty()) {
            const Font& f = rt.dw.fonts[fi];
            if (f.tpag >= 0 && f.tpag < (int)rt.dw.tpags.size()) {
                const Tpag& t = rt.dw.tpags[f.tpag];
                if (t.tex >= 0 && t.tex < (int)rt.dw.tex_ptrs.size()) {
                    const Image& pg = rt.page(t.tex);
                    bool transformed = (xscale != 1.0 || yscale != 1.0 || angle != 0.0);
                    double x = x0, y = y0;
                    for (char c : txt) {
                        if (c == '\n') { x = x0; y += std::max<uint32_t>(1, f.em) * yscale; continue; }
                        auto g = f.glyphs.find((uint8_t)c);
                        if (g == f.glyphs.end()) g = f.glyphs.find((uint16_t)63);
                        if (g == f.glyphs.end()) { x += 8 * xscale; continue; }
                        const Glyph& gl = g->second;
                        int g_sx = t.sx + gl.sx;
                        int g_sy = t.sy + gl.sy;
                        if (gl.sw > 0 && gl.sh > 0 && g_sx + gl.sw <= pg.w && g_sy + gl.sh <= pg.h) {
                            int base_dx = (int)std::lround(x + gl.offset * xscale) - rt.view_x;
                            int base_dy = (int)std::lround(y) - rt.view_y;
                            if (!transformed) {
                                rt.blit_sub_screen_tint(pg, g_sx, g_sy, gl.sw, gl.sh, base_dx, base_dy, alp, col);
                            } else {
                                int ow = (int)std::lround(gl.sw * xscale);
                                int oh = (int)std::lround(gl.sh * yscale);
                                // Rotate each glyph about the text origin (x0,y0) so the
                                // whole string stays coherent under one angle.
                                double ang = angle * 3.14159265358979 / 180.0;
                                double rx = (base_dx + rt.view_x) - x0;
                                double ry = (base_dy + rt.view_y) - y0;
                                int gx = (int)std::lround(x0 + rx * std::cos(ang) - ry * std::sin(ang)) - rt.view_x;
                                int gy = (int)std::lround(y0 + rx * std::sin(ang) + ry * std::cos(ang)) - rt.view_y;
                                rt.blit_sub_screen_scaled(pg, g_sx, g_sy, gl.sw, gl.sh, gx, gy, ow, oh, alp, col, angle);
                            }
                        }
                        x += gl.shift * xscale;
                    }
                    return vnum(0);
                }
            }
        }
        return vnum(0);
    };

    builtins["draw_text"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, -1, -1, -1, -1, -1); };
    builtins["draw_text_transformed"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, -1, -1, 3, 4, 5); };
    builtins["draw_text_ext"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, -1, -1, -1, -1, -1); };
    builtins["draw_text_color"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, 3, 7, -1, -1, -1); };
    builtins["draw_text_colour"] = builtins["draw_text_color"];
    builtins["draw_text_transformed_color"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, 6, 10, 3, 4, 5); };
    builtins["draw_text_transformed_colour"] = builtins["draw_text_transformed_color"];
    builtins["draw_text_ext_color"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, 5, 9, -1, -1, -1); };
    builtins["draw_text_ext_colour"] = builtins["draw_text_ext_color"];
    builtins["draw_text_ext_transformed"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, -1, -1, 5, 6, 7); };
    builtins["draw_text_ext_transformed_color"] = [=](Runtime& rt, std::vector<Value>& a){ return render_text_fn(rt, a, 8, 12, 5, 6, 7); };
    builtins["draw_text_ext_transformed_colour"] = builtins["draw_text_ext_transformed_color"];

    builtins["draw_rectangle"] = [](Runtime& rt, std::vector<Value>& a){
        if (!rt.screen || a.size() < 4) return vnum(0);
        int x1 = (int)to_num(a[0]) - rt.view_x, y1 = (int)to_num(a[1]) - rt.view_y;
        int x2 = (int)to_num(a[2]) - rt.view_x, y2 = (int)to_num(a[3]) - rt.view_y;
        bool outline = a.size() > 4 ? (to_num(a[4]) != 0) : false;
        if (x1 > x2) std::swap(x1, x2);
        if (y1 > y2) std::swap(y1, y2);
        uint8_t r = (uint8_t)(rt.draw_color & 0xFF);
        uint8_t g = (uint8_t)((rt.draw_color >> 8) & 0xFF);
        uint8_t b = (uint8_t)((rt.draw_color >> 16) & 0xFF);
        int sw = rt.screen->w, sh = rt.screen->h;
        if (outline) {
            for (int x = std::max(0, x1); x <= std::min(sw - 1, x2); ++x) {
                if (y1 >= 0 && y1 < sh) { uint8_t* p = &rt.screen->rgba[((size_t)y1 * sw + x) * 4]; p[0]=r; p[1]=g; p[2]=b; p[3]=255; }
                if (y2 >= 0 && y2 < sh) { uint8_t* p = &rt.screen->rgba[((size_t)y2 * sw + x) * 4]; p[0]=r; p[1]=g; p[2]=b; p[3]=255; }
            }
            for (int y = std::max(0, y1); y <= std::min(sh - 1, y2); ++y) {
                if (x1 >= 0 && x1 < sw) { uint8_t* p = &rt.screen->rgba[((size_t)y * sw + x1) * 4]; p[0]=r; p[1]=g; p[2]=b; p[3]=255; }
                if (x2 >= 0 && x2 < sw) { uint8_t* p = &rt.screen->rgba[((size_t)y * sw + x2) * 4]; p[0]=r; p[1]=g; p[2]=b; p[3]=255; }
            }
        } else {
            for (int y = std::max(0, y1); y <= std::min(sh - 1, y2); ++y) {
                for (int x = std::max(0, x1); x <= std::min(sw - 1, x2); ++x) {
                    uint8_t* p = &rt.screen->rgba[((size_t)y * sw + x) * 4];
                    p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                }
            }
        }
        return vnum(0);
    };
    builtins["draw_rectangle_color"] = builtins["draw_rectangle"];
    builtins["draw_rectangle_colour"] = builtins["draw_rectangle"];
    builtins["draw_line"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["draw_circle"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["draw_point"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["draw_set_font"] = [](Runtime& rt, std::vector<Value>& a){ rt.draw_font = (int)narg(a,0); return vnum(0); };
    builtins["draw_set_halign"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["draw_set_valign"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["draw_set_blend_mode"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };

    builtins["room_goto"] = [](Runtime& rt, std::vector<Value>& a){ rt.pending_room=(int)narg(a,0); return vnum(0); };
    builtins["room_goto_next"] = [](Runtime& rt, std::vector<Value>&){
        auto& o = rt.dw.room_order; int pos=(int)(std::find(o.begin(),o.end(),rt.room_index)-o.begin());
        rt.pending_room = (pos+1<(int)o.size()) ? (int)o[pos+1] : -1; return vnum(0); };
    builtins["room_goto_previous"] = [](Runtime& rt, std::vector<Value>&){
        auto& o = rt.dw.room_order; int pos=(int)(std::find(o.begin(),o.end(),rt.room_index)-o.begin());
        rt.pending_room = (pos>0) ? (int)o[pos-1] : -1; return vnum(0); };
    builtins["room_restart"] = [](Runtime& rt, std::vector<Value>&){ rt.pending_room=rt.room_index; return vnum(0); };
    builtins["game_end"] = [](Runtime& rt, std::vector<Value>&){ rt.running=false; return vnum(0); };
    builtins["game_restart"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };

    builtins["instance_create"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() < 3) return vnum(-4);
        double x = narg(a,0), y = narg(a,1);
        int obj = (int)narg(a,2);
        if (obj < 0 || obj >= (int)rt.dw.objects.size()) return vnum(-4);
        auto inst = std::make_unique<Instance>(rt.next_iid++, obj);
        inst->vars["x"] = vnum(x); inst->vars["y"] = vnum(y);
        inst->vars["xstart"] = vnum(x); inst->vars["ystart"] = vnum(y);
        inst->vars["image_xscale"] = vnum(1.0);
        inst->vars["image_yscale"] = vnum(1.0);
        inst->vars["image_angle"] = vnum(0.0);
        inst->vars["image_alpha"] = vnum(1.0);
        inst->vars["sprite_index"] = vnum(rt.dw.objects[obj].sprite);
        inst->vars["depth"] = vnum(rt.dw.objects[obj].depth);
        inst->vars["visible"] = vnum(rt.dw.objects[obj].visible ? 1 : 0);
        inst->vars["solid"] = vnum(rt.dw.objects[obj].solid ? 1 : 0);
        inst->vars["persistent"] = vnum(rt.dw.objects[obj].persistent ? 1 : 0);
        inst->vars["object_index"] = vnum(obj);
        inst->vars["id"] = vnum(inst->iid);
        Instance* ip = inst.get();
        rt.instances.push_back(std::move(inst));
        rt.run_event(ip, 0, 0);
        return vnum(ip->iid); };
    builtins["instance_exists"] = [](Runtime& rt, std::vector<Value>& a){
        int v=(int)narg(a,0); for (auto& i:rt.instances) if (i->alive && (i->iid==v||i->obj==v)) return vnum(1); return vnum(0); };
    builtins["instance_destroy"] = [](Runtime& rt, std::vector<Value>& a){
        auto kill = [&](Instance* inst) {
            if (inst && inst->alive) {
                rt.run_event(inst, 1, 0);
                inst->alive = false;
            }
        };
        if (a.empty()) {
            kill(rt.cur);
        } else {
            int target = (int)to_num(a[0]);
            if (target == -1) {
                kill(rt.cur);
            } else if (target == -2) {
                kill(rt.other);
            } else {
                for (auto& inst : rt.instances) {
                    if (inst->alive && (inst->iid == target || is_obj(rt.dw, inst.get(), target))) {
                        kill(inst.get());
                    }
                }
            }
        }
        return vnum(0);
    };
    builtins["instance_find"] = [](Runtime& rt, std::vector<Value>& a){
        int obj=(int)narg(a,0), n=(int)narg(a,1); int c=0;
        for (auto& i:rt.instances) if (i->alive&&i->obj==obj){ if (c==n) return vnum(i->iid); c++; } return vnum(-4); };
    builtins["instance_number"] = [](Runtime& rt, std::vector<Value>& a){
        int obj = (int)narg(a,0);
        if (obj == -1) { double c=0; for (auto& i:rt.instances) if (i->alive) c++; return vnum(c); }
        if (obj == -3) { double c=0; for (auto& i:rt.instances) if (!i->alive) c++; return vnum(c); }
        double c=0; for (auto& i:rt.instances) if (i->alive && is_obj(rt.dw, i.get(), obj)) c++;
        return vnum(c); };
    builtins["instance_create_depth"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() < 4) return vnum(-4);
        double x = narg(a,0), y = narg(a,1), depth = narg(a,2);
        int obj = (int)narg(a,3);
        if (obj < 0 || obj >= (int)rt.dw.objects.size()) return vnum(-4);
        auto inst = std::make_unique<Instance>(rt.next_iid++, obj);
        inst->vars["x"] = vnum(x); inst->vars["y"] = vnum(y);
        inst->vars["xstart"] = vnum(x); inst->vars["ystart"] = vnum(y);
        inst->vars["image_xscale"] = vnum(1.0);
        inst->vars["image_yscale"] = vnum(1.0);
        inst->vars["image_angle"] = vnum(0.0);
        inst->vars["image_alpha"] = vnum(1.0);
        inst->vars["sprite_index"] = vnum(rt.dw.objects[obj].sprite);
        inst->vars["depth"] = vnum(depth);
        inst->vars["visible"] = vnum(rt.dw.objects[obj].visible ? 1 : 0);
        inst->vars["solid"] = vnum(rt.dw.objects[obj].solid ? 1 : 0);
        inst->vars["persistent"] = vnum(rt.dw.objects[obj].persistent ? 1 : 0);
        inst->vars["object_index"] = vnum(obj);
        inst->vars["id"] = vnum(inst->iid);
        Instance* ip = inst.get();
        rt.instances.push_back(std::move(inst));
        rt.run_event(ip, 0, 0);
        return vnum(ip->iid); };
    builtins["instance_copy"] = [](Runtime& rt, std::vector<Value>& a){
        bool perf = narg(a,0) != 0.0;
        Instance* src = rt.cur;
        if (a.size() >= 2) { int t=(int)narg(a,1); for (auto& i:rt.instances) if (i->alive && (i->iid==t || i->obj==t)) { src=i.get(); break; } }
        if (!src) return vnum(-4);
        auto inst = std::make_unique<Instance>(rt.next_iid++, src->obj);
        inst->vars = src->vars; inst->arrays = src->arrays; inst->alarms = src->alarms;
        inst->vars["id"] = vnum(inst->iid);
        Instance* ip = inst.get();
        rt.instances.push_back(std::move(inst));
        if (perf) rt.run_event(ip, 0, 0);
        return vnum(ip->iid); };
    builtins["instance_exists"] = [](Runtime& rt, std::vector<Value>& a){
        int v=(int)narg(a,0);
        if (v == -1) { for (auto& i:rt.instances) if (i->alive) return vnum(1); return vnum(0); }
        for (auto& i:rt.instances) if (i->alive && (i->iid==v||i->obj==v)) return vnum(1);
        return vnum(0); };

    builtins["keyboard_check"] = [](Runtime& rt, std::vector<Value>& a){ return vnum(rt.keys_held.count((int)narg(a,0))?1:0); };
    builtins["keyboard_check_direct"] = builtins["keyboard_check"];
    builtins["keyboard_check_pressed"] = [](Runtime& rt, std::vector<Value>& a){ return vnum(rt.keys_pressed.count((int)narg(a,0))?1:0); };
    builtins["keyboard_check_released"] = [](Runtime& rt, std::vector<Value>& a){ return vnum(rt.keys_released.count((int)narg(a,0))?1:0); };
    builtins["keyboard_clear"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.empty()) {
            rt.keys_held.clear();
            rt.keys_pressed.clear();
        } else {
            int k = (int)to_num(a[0]);
            rt.keys_held.erase(k);
            rt.keys_pressed.erase(k);
        }
        return vnum(0);
    };
    builtins["io_clear"] = [](Runtime& rt, std::vector<Value>&){
        rt.keys_held.clear();
        rt.keys_pressed.clear();
        return vnum(0);
    };
    builtins["keyboard_key_press"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["keyboard_key_release"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };

    // NOTE: Undertale defines its own `control_init`/`control_update`/
    // `control_check`/`control_check_pressed`/`control_clear` scripts (CODE
    // 207-211). We must NOT shadow them with builtins, or the game's input
    // state machine breaks (e.g. the intro fade triggers immediately).
    // Runtime::call prefers scripts for these names.
    auto check_btn = [](const Runtime& rt, int btn, bool pressed) -> bool {
        const auto& set = pressed ? rt.keys_pressed : rt.keys_held;
        // 0 / gp_face1: Confirm (Z = 90, Enter = 13)
        if (btn == 0 || btn == 32769) return set.count(90) || set.count(13);
        // 1 / gp_face2: Cancel (X = 88, Shift = 16)
        if (btn == 1 || btn == 32770) return set.count(88) || set.count(16);
        // 2 / gp_face3 / gp_face4: Menu (C = 67, Ctrl = 17)
        if (btn == 2 || btn == 32771 || btn == 32772) return set.count(67) || set.count(17);
        // directions
        if (btn == 32781) return set.count(38) != 0; // up
        if (btn == 32782) return set.count(40) != 0; // down
        if (btn == 32783) return set.count(37) != 0; // left
        if (btn == 32784) return set.count(39) != 0; // right
        return false;
    };
    builtins["gamepad_button_check"] = [check_btn](Runtime& rt, std::vector<Value>& a){
        int btn = (int)(a.size() > 1 ? narg(a,1) : narg(a,0));
        return vnum(check_btn(rt, btn, false) ? 1 : 0);
    };
    builtins["gamepad_button_check_pressed"] = [check_btn](Runtime& rt, std::vector<Value>& a){
        int btn = (int)(a.size() > 1 ? narg(a,1) : narg(a,0));
        return vnum(check_btn(rt, btn, true) ? 1 : 0);
    };
    builtins["gamepad_button_check_any"] = [](Runtime& rt, std::vector<Value>&){
        return vnum((!rt.keys_pressed.empty() || !rt.keys_held.empty()) ? 1 : 0);
    };
    for (const char* n : {"joystick_exists","joystick_xpos",
                          "joystick_ypos","joystick_direction","joystick_pov","control_update"})
        builtins[n] = [](Runtime&, std::vector<Value>&){ return vnum(0); };

    builtins["script_execute"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.empty()) return vnum(0);
        const CodeEntry* e = nullptr;
        if (a[0].is_str()) { auto it=rt.scripts.find(a[0].str); if (it!=rt.scripts.end()) e=it->second; }
        else { int idx=(int)to_num(a[0]); if (idx>=0 && idx<(int)rt.dw.scripts.size()) {
            int cid=rt.dw.scripts[idx].second; if (cid>=0 && cid<(int)rt.dw.code.size()) e=&rt.dw.code[cid]; } }
        if (!e) return vnum(0);
        std::vector<Value> args(a.begin()+1, a.end());
        return rt.run_entry(e, args, rt.cur, rt.other); };
    builtins["event_user"] = [](Runtime& rt, std::vector<Value>& a){ if (rt.cur) rt.run_event(rt.cur, 7, 10+(int)narg(a,0)); return vnum(0); };
    builtins["event_perform"] = [](Runtime& rt, std::vector<Value>& a){ if (rt.cur) rt.run_event(rt.cur, (int)narg(a,0), (int)narg(a,1)); return vnum(0); };
    builtins["event_inherited"] = [](Runtime& rt, std::vector<Value>&){
        if (rt.event_stack.empty()) return vnum(0);
        EventContext ctx = rt.event_stack.back();
        if (!ctx.inst || ctx.cur_obj < 0 || ctx.cur_obj >= (int)rt.dw.objects.size()) return vnum(0);
        int parent_obj = rt.dw.objects[ctx.cur_obj].parent;
        while (parent_obj >= 0 && parent_obj < (int)rt.dw.objects.size()) {
            const ObjDef& po = rt.dw.objects[parent_obj];
            auto it = po.events.find(ctx.etype);
            if (it != po.events.end()) {
                auto jt = it->second.find(ctx.subtype);
                if (jt != it->second.end() && !jt->second.empty()) {
                    rt.event_stack.push_back({ctx.inst, parent_obj, ctx.etype, ctx.subtype});
                    std::vector<Value> noargs;
                    for (int ci : jt->second) {
                        if (ci >= 0 && ci < (int)rt.dw.code.size())
                            rt.run_entry(&rt.dw.code[ci], noargs, ctx.inst, ctx.inst);
                    }
                    rt.event_stack.pop_back();
                    return vnum(0);
                }
            }
            parent_obj = po.parent;
        }
        return vnum(0);
    };

    builtins["variable_global_exists"] = [](Runtime& rt, std::vector<Value>& a){
        std::string n=sarg(a,0); return vnum((rt.globals.count(n)||rt.global_builtins.count(n))?1:0); };
    builtins["variable_global_get"] = [](Runtime& rt, std::vector<Value>& a){
        std::string n=sarg(a,0); auto it=rt.globals.find(n); if(it!=rt.globals.end())return it->second;
        auto jt=rt.global_builtins.find(n); return jt!=rt.global_builtins.end()?jt->second:Value(0.0); };
    builtins["variable_global_set"] = [](Runtime& rt, std::vector<Value>& a){ rt.globals[sarg(a,0)]=a.size()>1?a[1]:Value(0.0); return vnum(0); };

    builtins["os_get_language"] = [](Runtime&, std::vector<Value>&){ return Value(std::string("en")); };
    builtins["os_get_region"] = [](Runtime&, std::vector<Value>&){ return Value(std::string("")); };
    builtins["os_is_paused"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["get_integer"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["get_string"] = [](Runtime&, std::vector<Value>& a){ return Value(sarg(a,1)); };

    builtins["sprite_prefetch"] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["sprite_get_width"] = [](Runtime& rt, std::vector<Value>& a){ int i=(int)narg(a,0); return vnum(i>=0&&i<(int)rt.dw.sprites.size()?rt.dw.sprites[i].width:0); };
    builtins["sprite_get_height"] = [](Runtime& rt, std::vector<Value>& a){ int i=(int)narg(a,0); return vnum(i>=0&&i<(int)rt.dw.sprites.size()?rt.dw.sprites[i].height:0); };
    builtins["sprite_get_number"] = [](Runtime& rt, std::vector<Value>& a){ int i=(int)narg(a,0); return vnum(i>=0&&i<(int)rt.dw.sprites.size()?rt.dw.sprites[i].frames.size():0); };

    builtins["string"] = [](Runtime&, std::vector<Value>& a){ return Value(a.empty()?std::string():to_str(a[0])); };
    builtins["real"] = [](Runtime&, std::vector<Value>& a){ return vnum(narg(a,0)); };
    builtins["string_length"] = [](Runtime&, std::vector<Value>& a){ return vnum((double)sarg(a,0).size()); };
    builtins["string_char_at"] = [](Runtime&, std::vector<Value>& a){
        int i=(int)narg(a,1);
        if (!a.empty() && a[0].is_arr()) {
            const std::vector<Value>& arr = *a[0].arrref;
            if (i>=0 && i<(int)arr.size()) return arr[i];
            if (i>=1 && i-1<(int)arr.size()) return arr[i-1];
            return Value(std::string(""));
        }
        std::string s=sarg(a,0);
        return Value((i>=1&&i<=(int)s.size())?std::string(1,s[i-1]):std::string()); };
    builtins["string_copy"] = [](Runtime&, std::vector<Value>& a){ std::string s=sarg(a,0); int i=(int)narg(a,1), n=(int)narg(a,2); if(i<1)i=1; return Value(s.substr(std::min((int)s.size(),i-1), n)); };
    builtins["substr"] = [](Runtime&, std::vector<Value>& a){ std::string s=sarg(a,0); int i=(int)narg(a,1); if(i<1)i=1; if(a.size()>=3) return Value(s.substr(std::min((int)s.size(),i-1), std::max(0,(int)narg(a,2)))); return Value(s.substr(std::min((int)s.size(),i-1))); };
    builtins["string_upper"] = [](Runtime&, std::vector<Value>& a){ std::string s=sarg(a,0); for(auto&c:s)c=(char)std::toupper((unsigned char)c); return Value(s); };
    builtins["string_lower"] = [](Runtime&, std::vector<Value>& a){ std::string s=sarg(a,0); for(auto&c:s)c=(char)std::tolower((unsigned char)c); return Value(s); };
    builtins["string_pos"] = [](Runtime&, std::vector<Value>& a){ std::string h=sarg(a,1), n=sarg(a,0); auto p=h.find(n); return vnum(p==std::string::npos?0:(double)p+1); };
    builtins["string_width"] = [](Runtime&, std::vector<Value>& a){ return vnum((double)sarg(a,0).size()*8); };
    builtins["string_height"] = [](Runtime&, std::vector<Value>&){ return vnum(16); };
    builtins["ord"] = [](Runtime&, std::vector<Value>& a){ std::string s=sarg(a,0); return vnum(s.empty()?0:(double)(unsigned char)s[0]); };
    builtins["chr"] = [](Runtime&, std::vector<Value>& a){ return Value(std::string(1,(char)(int)narg(a,0))); };
    builtins["is_undefined"] = [](Runtime&, std::vector<Value>& a){ return vnum((!a.empty() && a[0].type == Value::UNDEF) ? 1.0 : 0.0); };
    builtins["is_string"] = [](Runtime&, std::vector<Value>& a){ return vnum(a.size()&&a[0].is_str()?1:0); };
    builtins["is_real"] = [](Runtime&, std::vector<Value>& a){ return vnum((!a.empty() && a[0].is_num()) ? 1.0 : 0.0); };
    builtins["is_numeric"] = [](Runtime&, std::vector<Value>& a){ return vnum((!a.empty() && a[0].is_num()) ? 1.0 : 0.0); };
    builtins["is_bool"] = [](Runtime&, std::vector<Value>& a){
        if (a.empty()) return vnum(0);
        return vnum((a[0].is_num() && (a[0].num == 0.0 || a[0].num == 1.0)) ? 1.0 : 0.0); };
    builtins["is_array"] = [](Runtime&, std::vector<Value>&){ return vnum(0.0); };
    builtins["is_method"] = [](Runtime&, std::vector<Value>&){ return vnum(0.0); };

    builtins["abs"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::fabs(narg(a,0))); };
    builtins["sign"] = [](Runtime&, std::vector<Value>& a){ double v=narg(a,0); return vnum(v>0?1:(v<0?-1:0)); };
    builtins["floor"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::floor(narg(a,0))); };
    builtins["ceil"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::ceil(narg(a,0))); };
    builtins["round"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::floor(narg(a,0)+0.5)); };
    builtins["sqrt"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::sqrt(narg(a,0))); };
    builtins["sin"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::sin(narg(a,0))); };
    builtins["cos"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::cos(narg(a,0))); };
    builtins["tan"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::tan(narg(a,0))); };
    builtins["arctan"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::atan(narg(a,0))); };
    builtins["arcsin"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::asin(narg(a,0))); };
    builtins["arccos"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::acos(narg(a,0))); };
    builtins["arctan2"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::atan2(narg(a,0), narg(a,1))); };
    builtins["radtodeg"] = [](Runtime&, std::vector<Value>& a){ return vnum(narg(a,0) * 180.0 / M_PI); };
    builtins["dot_product"] = [](Runtime&, std::vector<Value>& a){ return vnum(narg(a,0)*narg(a,2) + narg(a,1)*narg(a,3)); };
    builtins["min"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::min(narg(a,0),narg(a,1))); };
    builtins["max"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::max(narg(a,0),narg(a,1))); };
    builtins["clamp"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::max(narg(a,1),std::min(narg(a,2),narg(a,0)))); };
    builtins["random"] = [](Runtime&, std::vector<Value>& a){ return vnum((double)std::rand()/RAND_MAX*narg(a,0)); };
    builtins["irandom"] = [](Runtime&, std::vector<Value>& a){ int m=(int)narg(a,0); return vnum(m>0?std::rand()%(m+1):0); };
    builtins["random_range"] = [](Runtime&, std::vector<Value>& a){
        double lo = narg(a,0), hi = narg(a,1);
        if (hi < lo) std::swap(lo, hi);
        return vnum(lo + (double)std::rand()/RAND_MAX * (hi - lo)); };
    builtins["irandom_range"] = [](Runtime&, std::vector<Value>& a){
        int lo = (int)narg(a,0), hi = (int)narg(a,1);
        if (hi < lo) std::swap(lo, hi);
        int span = hi - lo + 1;
        return vnum(span > 0 ? (lo + std::rand() % span) : lo); };
    builtins["random_set_seed"] = [](Runtime&, std::vector<Value>& a){ std::srand((unsigned)narg(a,0)); return vnum(0); };
    builtins["random_get_seed"] = [](Runtime&, std::vector<Value>&){ return vnum((double)std::rand()); };

    builtins["collision_rectangle"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(collision_rect(rt, narg(a,0),narg(a,1),narg(a,2),narg(a,3),(int)narg(a,4), a.size()>6?(int)narg(a,6):-1)); };
    builtins["collision_point"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(collision_rect(rt, narg(a,0),narg(a,1),narg(a,0),narg(a,1),(int)narg(a,2), a.size()>4?(int)narg(a,4):-1)); };
    builtins["collision_circle"] = [](Runtime& rt, std::vector<Value>& a){
        double r=narg(a,2); return vnum(collision_rect(rt,narg(a,0)-r,narg(a,1)-r,narg(a,0)+r,narg(a,1)+r,(int)narg(a,4),-1)); };
    builtins["collision_line"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(collision_rect(rt,narg(a,0),narg(a,1),narg(a,2),narg(a,3),(int)narg(a,4),-1)); };
    builtins["instance_place"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(instance_place_impl(rt,narg(a,0),narg(a,1),(int)narg(a,2))); };
    builtins["place_meeting"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(instance_place_impl(rt,narg(a,0),narg(a,1),(int)narg(a,2)) != -4.0 ? 1.0 : 0.0); };
    builtins["place_free"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(instance_place_impl(rt,narg(a,0),narg(a,1),(int)narg(a,2)) != -4.0 ? 0.0 : 1.0); };
    builtins["position_meeting"] = [](Runtime& rt, std::vector<Value>& a){
        int notme = rt.cur ? rt.cur->iid : -1;
        return vnum(collision_rect(rt,narg(a,0),narg(a,1),narg(a,0),narg(a,1),(int)narg(a,2),notme) != -4.0 ? 1.0 : 0.0); };

    // data structures
    builtins["ds_map_create"] = [](Runtime& rt, std::vector<Value>&){
        int id = rt.next_ds_map++;
        rt.ds_maps[id] = {};
        return vnum(id);
    };
    builtins["ds_map_destroy"] = [](Runtime& rt, std::vector<Value>& a){
        if (!a.empty()) rt.ds_maps.erase((int)narg(a,0));
        return vnum(0);
    };
    builtins["ds_map_add"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 3) {
            int id = (int)narg(a,0);
            std::string k = to_str(a[1]);
            rt.ds_maps[id][k] = a[2];
            return vnum(1);
        }
        return vnum(0);
    };
    builtins["ds_map_set"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 3) {
            int id = (int)narg(a,0);
            std::string k = to_str(a[1]);
            rt.ds_maps[id][k] = a[2];
        }
        return vnum(0);
    };
    builtins["ds_map_replace"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 3) {
            int id = (int)narg(a,0);
            std::string k = to_str(a[1]);
            rt.ds_maps[id][k] = a[2];
            return vnum(1);
        }
        return vnum(0);
    };
    builtins["ds_map_exists"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 2) {
            int id = (int)narg(a,0);
            auto it = rt.ds_maps.find(id);
            if (it != rt.ds_maps.end() && it->second.count(to_str(a[1]))) return vnum(1);
        }
        return vnum(0);
    };
    builtins["ds_map_find_value"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 2) {
            std::string k = to_str(a[1]);
            if (k == "instructions_confirm_key") return Value("[A]");
            if (k == "instructions_confirm_label") return Value("Confirm");
            if (k == "instructions_cancel_key") return Value("[B]");
            if (k == "instructions_cancel_label") return Value("Cancel");
            if (k == "instructions_menu_key") return Value("[X]");
            if (k == "instructions_menu_label") return Value("Menu");
            if (k == "instructions_quit_key") return Value("[START]");
            if (k == "instructions_quit_label") return Value("Quit");

            int id = (int)narg(a,0);
            auto it = rt.ds_maps.find(id);
            if (it != rt.ds_maps.end()) {
                auto jt = it->second.find(k);
                if (jt != it->second.end()) {
                    if (jt->second.is_str()) {
                        std::string s = jt->second.str;
                        auto rep = [&](const std::string& from, const std::string& to) {
                            size_t p = 0;
                            while ((p = s.find(from, p)) != std::string::npos) {
                                s.replace(p, from.size(), to);
                                p += to.size();
                            }
                        };
                        rep("[PRESS Z OR ENTER]", "[PRESS A]");
                        rep("[Z or ENTER]", "[A]");
                        rep("[X or SHIFT]", "[B]");
                        rep("[C or CTRL]", "[X]");
                        rep("[F4]", "");
                        rep("Z or ENTER", "A");
                        rep("X or SHIFT", "B");
                        rep("C or CTRL", "X");
                        return Value(s);
                    }
                    return jt->second;
                }
            }
        }
        Value v; v.type = Value::UNDEF; return v;
    };
    builtins["ds_list_create"] = [](Runtime& rt, std::vector<Value>&){
        int id = rt.next_ds_list++;
        rt.ds_lists[id] = {};
        return vnum(id);
    };
    builtins["ds_list_destroy"] = [](Runtime& rt, std::vector<Value>& a){
        if (!a.empty()) rt.ds_lists.erase((int)narg(a,0));
        return vnum(0);
    };
    builtins["ds_list_add"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 2) {
            int id = (int)narg(a,0);
            for (size_t i = 1; i < a.size(); ++i) rt.ds_lists[id].push_back(a[i]);
        }
        return vnum(0);
    };
    builtins["ds_list_size"] = [](Runtime& rt, std::vector<Value>& a){
        if (!a.empty()) {
            auto it = rt.ds_lists.find((int)narg(a,0));
            if (it != rt.ds_lists.end()) return vnum(it->second.size());
        }
        return vnum(0);
    };
    builtins["ds_list_find_value"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 2) {
            auto it = rt.ds_lists.find((int)narg(a,0));
            if (it != rt.ds_lists.end()) {
                int idx = (int)narg(a,1);
                if (idx >= 0 && idx < (int)it->second.size()) return it->second[idx];
            }
        }
        Value v; v.type = Value::UNDEF; return v;
    };
    builtins["ds_list_clear"] = [](Runtime& rt, std::vector<Value>& a){
        if (!a.empty()) rt.ds_lists[(int)narg(a,0)].clear();
        return vnum(0);
    };

    for (const char* n : {"file_exists","ini_open",
                          "ini_close","ini_read_real","ini_read_string","ini_section_exists",
                          "ossafe_ini_open","ossafe_ini_close","randomize",
                          "application_surface_enable",
                          "application_surface_draw_enable","display_set_gui_size","window_set_fullscreen",
                          "window_set_caption","texture_set_interpolation","draw_enable_alphablend",
                          "show_debug_message","show_message","screen_refresh","set_automatic_draw",
                          "instance_deactivate_all","instance_activate_all",
                          "file_text_open_write","file_text_write_string","file_text_close",
                          "file_delete","file_rename","steam_initialised","steam_file_exists",
                          "steam_file_delete","trophy_init","action_kill_object","action_move_to"})
        if (!builtins.count(n)) builtins[n] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["ini_read_string"] = [](Runtime&, std::vector<Value>&){ return Value(std::string("")); };
    builtins["surface_get_width"] = [](Runtime& rt, std::vector<Value>&){
        auto it = rt.global_arrays.find("view_wview");
        if (it != rt.global_arrays.end() && !it->second.empty() && to_num(it->second[0]) > 0)
            return it->second[0];
        return vnum(320.0);
    };
    builtins["surface_get_height"] = [](Runtime& rt, std::vector<Value>&){
        auto it = rt.global_arrays.find("view_hview");
        if (it != rt.global_arrays.end() && !it->second.empty() && to_num(it->second[0]) > 0)
            return it->second[0];
        return vnum(240.0);
    };

    // ---- bulk additions: strings / math / colour ----
    builtins["string_replace"] = [](Runtime&, std::vector<Value>& a){
        std::string s = sarg(a,0), from = sarg(a,1), to = sarg(a,2);
        if (from.empty()) return Value(s);
        size_t f = s.find(from);
        if (f == std::string::npos) return vnum(0);
        s.replace(f, from.size(), to);
        return Value(s);
    };
    builtins["string_replace_all"] = [](Runtime&, std::vector<Value>& a){
        std::string s = sarg(a,0), from = sarg(a,1), to = sarg(a,2);
        if (from.empty()) return Value(s);
        std::string out; size_t pos = 0, f;
        while ((f = s.find(from, pos)) != std::string::npos) { out.append(s, pos, f - pos); out += to; pos = f + from.size(); }
        out.append(s, pos, std::string::npos);
        return Value(out);
    };
    builtins["string_delete"] = [](Runtime&, std::vector<Value>& a){
        std::string s = sarg(a,0); int pos = (int)narg(a,1), cnt = (int)narg(a,2);
        if (pos < 1 || pos > (int)s.size() || cnt <= 0) return Value(s);
        s.erase((size_t)pos - 1, (size_t)cnt);
        return Value(s);
    };
    builtins["string_repeat"] = [](Runtime&, std::vector<Value>& a){
        std::string s = sarg(a,0), out; int n = (int)narg(a,1);
        for (int i = 0; i < n; ++i) out += s;
        return Value(out);
    };
    builtins["string_byte_length"] = [](Runtime&, std::vector<Value>& a){ return vnum((double)sarg(a,0).size()); };
    builtins["choose"] = [](Runtime&, std::vector<Value>& a){
        if (a.empty()) return vnum(0);
        return a[(size_t)std::rand() % a.size()];
    };
    builtins["power"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::pow(narg(a,0), narg(a,1))); };
    builtins["sqr"] = [](Runtime&, std::vector<Value>& a){ return vnum(narg(a,0) * narg(a,0)); };
    builtins["degtorad"] = [](Runtime&, std::vector<Value>& a){ return vnum(narg(a,0) * M_PI / 180.0); };
    builtins["darctan2"] = [](Runtime&, std::vector<Value>& a){ return vnum(std::atan2(narg(a,0), narg(a,1)) * 180.0 / M_PI); };
    builtins["lengthdir_x"] = [](Runtime&, std::vector<Value>& a){ return vnum(narg(a,0) * std::cos(narg(a,1) * M_PI / 180.0)); };
    builtins["lengthdir_y"] = [](Runtime&, std::vector<Value>& a){ return vnum(-narg(a,0) * std::sin(narg(a,1) * M_PI / 180.0)); };
    builtins["point_direction"] = [](Runtime&, std::vector<Value>& a){
        double d = std::atan2(-(narg(a,3) - narg(a,1)), narg(a,2) - narg(a,0)) * 180.0 / M_PI;
        if (d < 0) d += 360.0;
        return vnum(d);
    };
    builtins["point_distance"] = [](Runtime&, std::vector<Value>& a){
        return vnum(std::hypot(narg(a,2) - narg(a,0), narg(a,3) - narg(a,1)));
    };
    builtins["distance_to_point"] = [](Runtime& rt, std::vector<Value>& a){
        if (!rt.cur) return vnum(0);
        double x = to_num(rt.cur->vars.count("x") ? rt.cur->vars["x"] : Value(0.0));
        double y = to_num(rt.cur->vars.count("y") ? rt.cur->vars["y"] : Value(0.0));
        return vnum(std::hypot(narg(a,0) - x, narg(a,1) - y));
    };
    builtins["distance_to_object"] = [](Runtime& rt, std::vector<Value>& a){
        if (!rt.cur) return vnum(100000);
        double x = to_num(rt.cur->vars.count("x") ? rt.cur->vars["x"] : Value(0.0));
        double y = to_num(rt.cur->vars.count("y") ? rt.cur->vars["y"] : Value(0.0));
        double best = 100000; int target = (int)narg(a,0);
        for (auto& i : rt.instances) {
            if (!i->alive || i.get() == rt.cur || !is_obj(rt.dw, i.get(), target)) continue;
            double ix = to_num(i->vars.count("x") ? i->vars["x"] : Value(0.0));
            double iy = to_num(i->vars.count("y") ? i->vars["y"] : Value(0.0));
            best = std::min(best, std::hypot(ix - x, iy - y));
        }
        return vnum(best);
    };
    builtins["move_towards_point"] = [](Runtime& rt, std::vector<Value>& a){
        if (!rt.cur) return vnum(0);
        double x = to_num(rt.cur->vars.count("x") ? rt.cur->vars["x"] : Value(0.0));
        double y = to_num(rt.cur->vars.count("y") ? rt.cur->vars["y"] : Value(0.0));
        double dir = std::atan2(-(narg(a,1) - y), narg(a,0) - x);
        double sp = narg(a,2);
        double deg = dir * 180.0 / M_PI; if (deg < 0) deg += 360.0;
        rt.cur->vars["direction"] = vnum(deg);
        rt.cur->vars["speed"] = vnum(sp);
        rt.cur->vars["hspeed"] = vnum(sp * std::cos(dir));
        rt.cur->vars["vspeed"] = vnum(-sp * std::sin(dir));
        return vnum(0);
    };
    builtins["make_color_rgb"] = [](Runtime&, std::vector<Value>& a){
        return vnum((double)(((int)narg(a,0) & 255) | (((int)narg(a,1) & 255) << 8) | (((int)narg(a,2) & 255) << 16)));
    };
    builtins["make_colour_rgb"] = builtins["make_color_rgb"];
    builtins["make_color_hsv"] = [](Runtime&, std::vector<Value>& a){
        double h = narg(a,0) / 255.0 * 6.0, s = narg(a,1) / 255.0, v = narg(a,2) / 255.0;
        int i = (int)std::floor(h); double f = h - i;
        double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f)), r, g, b;
        switch (((i % 6) + 6) % 6) {
            case 0: r=v; g=t; b=p; break; case 1: r=q; g=v; b=p; break; case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break; case 4: r=t; g=p; b=v; break; default: r=v; g=p; b=q; break;
        }
        return vnum((double)((int)(r*255) | ((int)(g*255) << 8) | ((int)(b*255) << 16)));
    };
    builtins["make_colour_hsv"] = builtins["make_color_hsv"];
    builtins["merge_color"] = [](Runtime&, std::vector<Value>& a){
        int c1 = (int)narg(a,0), c2 = (int)narg(a,1); double t = narg(a,2);
        auto mix = [&](int sh){ return (int)(((c1 >> sh) & 255) * (1 - t) + ((c2 >> sh) & 255) * t) & 255; };
        return vnum((double)(mix(0) | (mix(8) << 8) | (mix(16) << 16)));
    };
    builtins["merge_colour"] = builtins["merge_color"];

    // ---- rooms / instances / ds ----
    builtins["room_next"] = [](Runtime& rt, std::vector<Value>& a){
        int r = (int)narg(a,0) + 1; return vnum(r < (int)rt.dw.rooms.size() ? r : -1); };
    builtins["room_previous"] = [](Runtime&, std::vector<Value>& a){
        int r = (int)narg(a,0) - 1; return vnum(r >= 0 ? r : -1); };
    builtins["room_get_name"] = [](Runtime& rt, std::vector<Value>& a){
        int r = (int)narg(a,0); return Value(r >= 0 && r < (int)rt.dw.rooms.size() ? rt.dw.rooms[r].name : std::string("")); };
    builtins["sprite_exists"] = [](Runtime& rt, std::vector<Value>& a){
        int s = (int)narg(a,0); return vnum(s >= 0 && s < (int)rt.dw.sprites.size() ? 1 : 0); };
    builtins["sprite_get_name"] = [](Runtime& rt, std::vector<Value>& a){
        int s = (int)narg(a,0); return Value(s >= 0 && s < (int)rt.dw.sprites.size() ? rt.dw.sprites[s].name : std::string("")); };
    builtins["background_get_width"] = [](Runtime& rt, std::vector<Value>& a){
        int b = (int)narg(a,0); if (b < 0 || b >= (int)rt.dw.bgnds.size() || rt.dw.bgnds[b].tpag < 0) return vnum(0);
        return vnum(rt.dw.tpags[rt.dw.bgnds[b].tpag].sw); };
    builtins["background_get_height"] = [](Runtime& rt, std::vector<Value>& a){
        int b = (int)narg(a,0); if (b < 0 || b >= (int)rt.dw.bgnds.size() || rt.dw.bgnds[b].tpag < 0) return vnum(0);
        return vnum(rt.dw.tpags[rt.dw.bgnds[b].tpag].sh); };
    builtins["instance_position"] = [](Runtime& rt, std::vector<Value>& a){
        return vnum(collision_rect(rt, narg(a,0), narg(a,1), narg(a,0), narg(a,1), (int)narg(a,2), false)); };
    builtins["ds_map_delete"] = [](Runtime& rt, std::vector<Value>& a){
        if (a.size() >= 2) { auto it = rt.ds_maps.find((int)narg(a,0)); if (it != rt.ds_maps.end()) it->second.erase(to_str(a[1])); }
        return vnum(0); };
    builtins["window_get_width"] = [](Runtime&, std::vector<Value>&){ return vnum(320); };
    builtins["window_get_height"] = [](Runtime&, std::vector<Value>&){ return vnum(240); };

    // ---- drawing ----
    builtins["draw_background"] = [](Runtime& rt, std::vector<Value>& a){
        int b = (int)narg(a,0);
        if (b < 0 || b >= (int)rt.dw.bgnds.size()) return vnum(0);
        int tp = rt.dw.bgnds[b].tpag; if (tp < 0 || tp >= (int)rt.dw.tpags.size()) return vnum(0);
        const Tpag& t = rt.dw.tpags[tp];
        if (t.tex < 0 || t.tex >= (int)rt.dw.tex_ptrs.size()) return vnum(0);
        const Image& pg = rt.page(t.tex);
        if (pg.w <= 0 || t.sx + t.sw > pg.w || t.sy + t.sh > pg.h) return vnum(0);
        rt.blit_sub_screen(pg, t.sx, t.sy, t.sw, t.sh, (int)narg(a,1) + t.tx - rt.view_x, (int)narg(a,2) + t.ty - rt.view_y, rt.draw_alpha);
        return vnum(0);
    };
    builtins["draw_sprite_part"] = [](Runtime& rt, std::vector<Value>& a){
        int si = (int)narg(a,0); if (si < 0 || si >= (int)rt.dw.sprites.size()) return vnum(0);
        const Sprite& sp = rt.dw.sprites[si]; if (sp.frames.empty()) return vnum(0);
        int n = (int)sp.frames.size(); int fi = (((int)narg(a,1)) % n + n) % n;
        int tp = sp.frames[fi]; if (tp < 0 || tp >= (int)rt.dw.tpags.size()) return vnum(0);
        const Tpag& t = rt.dw.tpags[tp];
        if (t.tex < 0 || t.tex >= (int)rt.dw.tex_ptrs.size()) return vnum(0);
        const Image& pg = rt.page(t.tex); if (pg.w <= 0) return vnum(0);
        int l = (int)narg(a,2), tt = (int)narg(a,3), w = (int)narg(a,4), h = (int)narg(a,5);
        rt.blit_sub_screen(pg, t.sx + l, t.sy + tt, w, h, (int)narg(a,6) - rt.view_x, (int)narg(a,7) - rt.view_y, rt.draw_alpha);
        return vnum(0);
    };
    builtins["draw_sprite_part_ext"] = builtins["draw_sprite_part"];

    // ================= AUDIO =================
    // Semantics: GameMaker Studio HTML5 runtime (Function_Sound.js +
    // Function_Sound_Legacy.js). An argument is either an SOND asset index
    // (0-based) or a voice handle >= kHandleBase (300000).
    builtins["audio_play_sound"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.audio_register_clips();
        int asset = (a.size() > 0) ? (int)narg(a,0) : -1;
        double priority = narg(a,1);
        bool loop = to_bool(a.size() > 2 ? a[2] : Value(0.0));
        double gain = (a.size() > 3) ? narg(a,3) : 1.0;
        double offset = (a.size() > 4) ? narg(a,4) : 0.0;
        double pitch = (a.size() > 5) ? narg(a,5) : 1.0;
        int h = rt.audio->play(asset, loop, gain, offset, pitch, priority);
        return vnum(h);
    };
    builtins["audio_play_sound_at"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        // positional playback; 3DS/host have no spatialisation -> non-positional.
        rt.audio_register_clips();
        int asset = (int)narg(a,0);
        bool loop = to_bool(a.size() > 7 ? a[7] : Value(0.0));
        double priority = (a.size() > 8) ? narg(a,8) : 1.0;
        double gain = (a.size() > 9) ? narg(a,9) : 1.0;
        double offset = (a.size() > 10) ? narg(a,10) : 0.0;
        double pitch = (a.size() > 11) ? narg(a,11) : 1.0;
        return vnum(rt.audio->play(asset, loop, gain, offset, pitch, priority));
    };
    builtins["audio_stop_sound"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0);
        if (id >= kHandleBase) rt.audio->stop_handle(id); else rt.audio->stop_asset(id);
        return vnum(0);
    };
    builtins["audio_stop_all"] = [](Runtime& rt, std::vector<Value>&) -> Value {
        rt.ensure_audio(); rt.audio->stop_all(); return vnum(0); };
    builtins["audio_pause_sound"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0);
        if (id >= kHandleBase) rt.audio->pause_handle(id); else rt.audio->pause_asset(id);
        return vnum(0);
    };
    builtins["audio_resume_sound"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0);
        if (id >= kHandleBase) rt.audio->resume_handle(id); else rt.audio->resume_asset(id);
        return vnum(0);
    };
    builtins["audio_pause_all"] = [](Runtime& rt, std::vector<Value>&) -> Value {
        rt.ensure_audio(); rt.audio->pause_all(); return vnum(0); };
    builtins["audio_resume_all"] = [](Runtime& rt, std::vector<Value>&) -> Value {
        rt.ensure_audio(); rt.audio->resume_all(); return vnum(0); };
    builtins["audio_sound_gain"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0); double g = narg(a,1);
        if (g < 0.0) g = 0.0;
        if (id >= kHandleBase) rt.audio->set_gain_handle(id, g); else rt.audio->set_gain_asset(id, g);
        return vnum(0);
    };
    builtins["audio_sound_get_gain"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0);
        return vnum(id >= kHandleBase ? rt.audio->get_gain_handle(id) : rt.audio->get_gain_asset(id));
    };
    builtins["audio_sound_pitch"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0); double p = narg(a,1);
        if (p <= 0.0) p = 0.0000001;
        if (id >= kHandleBase) rt.audio->set_pitch_handle(id, p); else rt.audio->set_pitch_asset(id, p);
        return vnum(0);
    };
    builtins["audio_sound_get_pitch"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0);
        return vnum(id >= kHandleBase ? rt.audio->get_pitch_handle(id) : rt.audio->get_pitch_asset(id));
    };
    builtins["audio_is_playing"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); return vnum(rt.audio->is_playing((int)narg(a,0)) ? 1.0 : 0.0); };
    builtins["audio_is_paused"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); return vnum(rt.audio->is_paused((int)narg(a,0)) ? 1.0 : 0.0); };
    builtins["audio_sound_get_track_position"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); return vnum(rt.audio->get_track_position((int)narg(a,0))); };
    builtins["audio_sound_set_track_position"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); rt.audio->set_track_position((int)narg(a,0), narg(a,1)); return vnum(0); };
    builtins["audio_master_gain"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); rt.audio->set_master_gain(narg(a,0)); return vnum(0); };
    builtins["audio_set_master_gain"] = builtins["audio_master_gain"];
    builtins["audio_get_master_gain"] = [](Runtime& rt, std::vector<Value>&) -> Value {
        rt.ensure_audio(); return vnum(rt.audio->master_gain()); };
    builtins["audio_channel_num"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); rt.audio->set_max_voices((int)narg(a,0)); return vnum(0); };
    builtins["audio_exists"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.ensure_audio(); int id = (int)narg(a,0);
        if (id >= kHandleBase) return vnum(rt.audio->is_playing(id) ? 1.0 : 0.0);
        return vnum(rt.audio->has_clip(id) ? 1.0 : 0.0);
    };
    builtins["audio_sound_length"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        rt.audio_register_clips(); int asset = (int)narg(a,0);
        if (asset >= kHandleBase) return vnum(-1.0);
        std::vector<uint8_t> bytes;
        if (asset < 0 || asset >= (int)rt.dw.sounds.size()) return vnum(-1.0);
        const Sound& s = rt.dw.sounds[asset];
        if (s.audo_id < 0) return vnum(-1.0);
        bytes = rt.dw.audio_bytes(s.audo_id);
        AudioClip clip;
        if (!decode_audio(bytes.data(), bytes.size(), clip)) return vnum(-1.0);
        return vnum(clip.duration());
    };
    // Legacy sound_*/snd_* API: operate on asset index (no handle).
    builtins["snd_play"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        int asset = (int)narg(a,0);
        rt.audio_register_clips();
        return vnum(rt.audio->play(asset, false, 1.0, 0.0, 1.0, 1.0));
    };
    builtins["snd_loop"] = [](Runtime& rt, std::vector<Value>& a) -> Value {
        int asset = (int)narg(a,0);
        rt.audio_register_clips();
        return vnum(rt.audio->play(asset, true, 1.0, 0.0, 1.0, 1.0));
    };
    builtins["snd_stop"]     = builtins["audio_stop_sound"];
    builtins["snd_stop_all"] = builtins["audio_stop_all"];
    builtins["sound_play"]   = builtins["snd_play"];
    builtins["sound_loop"]   = builtins["snd_loop"];
    builtins["sound_stop"]   = builtins["snd_stop"];
    builtins["sound_stop_all"] = builtins["snd_stop_all"];
    builtins["sound_isplaying"] = builtins["audio_is_playing"];
    builtins["sound_volume"] = builtins["audio_sound_gain"];
    builtins["sound_global_volume"] = builtins["audio_master_gain"];

    // ---- harmless stubs (no 3DS equivalent / not needed yet) ----
    for (const char* n : {"ini_write_real","ini_write_string","ini_open_from_string","sprite_replace","sprite_delete",
        "sprite_create_from_surface","sprite_collision_mask","path_start","path_end","tile_layer_shift","tile_layer_hide",
        "tile_layer_show","move_snap","draw_line_width","draw_line_width_color","draw_line_color","draw_set_circle_precision",
        "draw_ellipse_color","draw_triangle","draw_triangle_color","draw_roundrect","draw_point_color","draw_clear_alpha",
        "draw_background_part_ext","draw_background_stretched","draw_surface","draw_surface_ext",
        "window_set_position","window_center","steam_file_write_file",
        "file_text_writeln","file_text_write_real","surface_set_target","surface_reset_target","surface_free",
        "buffer_async_group_option","buffer_async_group_begin","buffer_async_group_end","buffer_write","buffer_save_async",
        "buffer_load_async","buffer_delete","instance_change","instance_activate_object","room_set_persistent",
        "action_set_alarm","action_set_relative","action_move","action_set_hspeed","action_set_motion","action_create_object",
        "action_set_gravity","action_set_friction","action_previous_room","action_move_point","get_string_async",
        "joystick_check_button","joystick_has_pov","gamepad_axis_value","extension_stubfunc_real","ds_map_set_post"})
        if (!builtins.count(n)) builtins[n] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    for (const char* n : {"gamepad_is_connected","window_get_fullscreen"})
        if (!builtins.count(n)) builtins[n] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    builtins["file_text_eof"] = [](Runtime&, std::vector<Value>&){ return vnum(1); };
    for (const char* n : {"joystick_buttons","gamepad_get_device_count","window_get_x","window_get_y","buffer_create",
        "buffer_get_size","buffer_read","surface_create",
        "draw_getpixel","date_current_datetime","json_decode","json_encode"})
        if (!builtins.count(n)) builtins[n] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
    for (const char* n : {"file_text_open_read","file_text_read_string","file_text_readln","file_text_read_real"})
        if (!builtins.count(n)) builtins[n] = [](Runtime&, std::vector<Value>&){ return vnum(0); };
}

// ---------------- room / event lifecycle ----------------
void Runtime::run_event(Instance* inst, int etype, int subtype) {
    if (!inst || inst->obj < 0 || inst->obj >= (int)dw.objects.size()) return;
    int cur_obj = inst->obj;
    while (cur_obj >= 0 && cur_obj < (int)dw.objects.size()) {
        const ObjDef& o = dw.objects[cur_obj];
        auto it = o.events.find(etype);
        if (it != o.events.end()) {
            auto jt = it->second.find(subtype);
            if (jt != it->second.end() && !jt->second.empty()) {
                if (diag_sink) {
                    std::string l = "event obj#" + std::to_string(inst->iid) + " " +
                                    dw.objects[cur_obj].name + " etype=" + std::to_string(etype) +
                                    " sub=" + std::to_string(subtype);
                    diag_sink(l.c_str());
                }
                event_stack.push_back({inst, cur_obj, etype, subtype});
                std::vector<Value> noargs;
                for (int ci : jt->second) {
                    if (ci >= 0 && ci < (int)dw.code.size())
                        run_entry(&dw.code[ci], noargs, inst, inst);
                }
                event_stack.pop_back();
                return;
            }
        }
        cur_obj = o.parent;
    }
}

void Runtime::start_room(int index, std::vector<std::unique_ptr<Instance>> keep) {
    pending_room = -1;
    room_index = index;
    instances = std::move(keep);
    const Room& room = dw.rooms[index];

    globals["room_width"] = vnum(room.width);
    globals["room_height"] = vnum(room.height);
    globals["room_speed"] = vnum(room.speed ? room.speed : 30);
    globals["view_enabled"] = vnum((room.flags & 1) ? 1 : 0);
    globals["view_current"] = vnum(0);

    auto& vx = global_arrays["view_xview"]; vx.assign(8, vnum(0.0));
    auto& vy = global_arrays["view_yview"]; vy.assign(8, vnum(0.0));
    auto& vw = global_arrays["view_wview"]; vw.assign(8, vnum(room.width ? room.width : 320));
    auto& vh = global_arrays["view_hview"]; vh.assign(8, vnum(room.height ? room.height : 240));
    auto& vv = global_arrays["view_visible"]; vv.assign(8, vnum(0.0));
    auto& vo = global_arrays["view_object"]; vo.assign(8, vnum(-1.0));
    auto& vpx = global_arrays["view_xport"]; vpx.assign(8, vnum(0.0));
    auto& vpy = global_arrays["view_yport"]; vpy.assign(8, vnum(0.0));
    auto& vpw = global_arrays["view_wport"]; vpw.assign(8, vnum(room.width ? room.width : 320));
    auto& vph = global_arrays["view_hport"]; vph.assign(8, vnum(room.height ? room.height : 240));
    auto& vhb = global_arrays["view_hborder"]; vhb.assign(8, vnum(0.0));
    auto& vvb = global_arrays["view_vborder"]; vvb.assign(8, vnum(0.0));
    auto& vhs = global_arrays["view_hspeed"]; vhs.assign(8, vnum(-1.0));
    auto& vvs = global_arrays["view_vspeed"]; vvs.assign(8, vnum(-1.0));

    if (!room.views.empty()) {
        for (size_t vi = 0; vi < room.views.size() && vi < 8; ++vi) {
            const auto& v = room.views[vi];
            vv[vi] = vnum(v.visible ? 1.0 : 0.0);
            vx[vi] = vnum(v.x);
            vy[vi] = vnum(v.y);
            vw[vi] = vnum(v.w);
            vh[vi] = vnum(v.h);
            vpx[vi] = vnum(v.port_x);
            vpy[vi] = vnum(v.port_y);
            vpw[vi] = vnum(v.port_w);
            vph[vi] = vnum(v.port_h);
            vhb[vi] = vnum(v.border_x);
            vvb[vi] = vnum(v.border_y);
            vhs[vi] = vnum(v.speed_x);
            vvs[vi] = vnum(v.speed_y);
            vo[vi] = vnum(v.follow_object);
        }
    } else {
        vv[0] = vnum(1.0);
    }

    for (const auto& ri : room.instances) {
        if (ri.obj < 0 || ri.obj >= (int)dw.objects.size()) continue;
        auto inst = std::make_unique<Instance>(ri.iid, ri.obj);
        inst->vars["x"] = vnum(ri.x); inst->vars["y"] = vnum(ri.y);
        inst->vars["xstart"] = vnum(ri.x); inst->vars["ystart"] = vnum(ri.y);
        inst->vars["image_xscale"] = vnum(ri.scale_x);
        inst->vars["image_yscale"] = vnum(ri.scale_y);
        inst->vars["image_angle"] = vnum(-ri.rotation);
        inst->vars["sprite_index"] = vnum(dw.objects[ri.obj].sprite);
        inst->vars["depth"] = vnum(dw.objects[ri.obj].depth);
        inst->vars["visible"] = vnum(dw.objects[ri.obj].visible ? 1 : 0);
        inst->vars["solid"] = vnum(dw.objects[ri.obj].solid ? 1 : 0);
        inst->vars["persistent"] = vnum(dw.objects[ri.obj].persistent ? 1 : 0);
        inst->vars["object_index"] = vnum(ri.obj);
        inst->vars["id"] = vnum(ri.iid);
        instances.push_back(std::move(inst));
        Instance* ip = instances.back().get();
        if (ri.creation_code >= 0 && ri.creation_code < (int)dw.code.size()) {
            std::vector<Value> noargs; run_entry(&dw.code[ri.creation_code], noargs, ip, ip);
        }
        run_event(ip, 0, 0);
    }
    if (room.creation_code >= 0 && room.creation_code < (int)dw.code.size() && !instances.empty()) {
        std::vector<Value> noargs;
        run_entry(&dw.code[room.creation_code], noargs, instances[0].get(), instances[0].get());
    }
    static bool booted = false;
    if (!booted) {
        booted = true;
        for (size_t i = 0; i < instances.size(); ++i)
            if (instances[i]->alive) run_event(instances[i].get(), 7, 2);
    }
    for (size_t i = 0; i < instances.size(); ++i)
        if (instances[i]->alive) run_event(instances[i].get(), 7, 4);
    if (fast_boot) {
        for (size_t i = 0; i < instances.size(); ++i)
            if (dw.objects[instances[i]->obj].name == "obj_time") instances[i]->vars["started"] = vnum(1.0);
    }
    globals["room"] = vnum(index);
    global_builtins["current_time"] = globals.count("__t") ? globals["__t"] : Value(0.0);
}

void Runtime::change_room(int index) {
    for (size_t i = 0; i < instances.size(); ++i)
        if (instances[i]->alive) run_event(instances[i].get(), 7, 5);
    if (index < 0 || index >= (int)dw.rooms.size()) { running = false; return; }
    std::vector<std::unique_ptr<Instance>> keep;
    for (auto& i : instances) {
        if (!i || !i->alive) continue;
        bool pers = dw.objects[i->obj].persistent;
        if (i->vars.count("persistent")) pers = to_bool(i->vars["persistent"]);
        if (pers) keep.push_back(std::move(i));
    }
    start_room(index, std::move(keep));
}

void Runtime::step() {
    frame_count++;
    double ms = frame_count * (1000.0 / std::max(1, (int)(dw.rooms[room_index].speed ? dw.rooms[room_index].speed : 60)));
    global_builtins["current_time"] = vnum(ms);
    // alarms
    for (size_t j = 0; j < instances.size(); ++j) {
        Instance* inst = instances[j].get();
        if (!inst || !inst->alive) continue;
        for (int i = 0; i < 12; ++i) {
            if (inst->alarms[i] < 0) continue;
            // Official GM semantics (Events.js HandleAlarm): decrement first,
            // then fire if it reached exactly 0 in this step.
            inst->alarms[i]--;
            if (inst->alarms[i] == 0) {
                inst->alarms[i] = -1;
                run_event(inst, 2, i);
            }
        }
    }
    for (int sub : {1, 0}) {
        size_t n = instances.size();
        for (size_t j = 0; j < n && j < instances.size(); ++j) {
            Instance* inst = instances[j].get();
            if (inst && inst->alive) run_event(inst, 3, sub);
        }
    }

    // Motion & Animation advancement (between Step and End Step / collisions)
    for (size_t j = 0; j < instances.size(); ++j) {
        Instance* inst = instances[j].get();
        if (!inst || !inst->alive) continue;
        auto& v = inst->vars;
        double cur_x = v.count("x") ? to_num(v["x"]) : 0.0;
        double cur_y = v.count("y") ? to_num(v["y"]) : 0.0;
        v["xprevious"] = Value(cur_x);
        v["yprevious"] = Value(cur_y);

        double hs = v.count("hspeed") ? to_num(v["hspeed"]) : 0.0;
        double vs = v.count("vspeed") ? to_num(v["vspeed"]) : 0.0;
        if (hs != 0.0) v["x"] = Value(cur_x + hs);
        if (vs != 0.0) v["y"] = Value(cur_y + vs);

        double img_spd = v.count("image_speed") ? to_num(v["image_speed"]) : 1.0;
        double img_idx = v.count("image_index") ? to_num(v["image_index"]) : 0.0;
        v["image_index"] = Value(img_idx + img_spd);
    }

    // End step (subtype 2)
    {
        size_t n = instances.size();
        for (size_t j = 0; j < n && j < instances.size(); ++j) {
            Instance* inst = instances[j].get();
            if (inst && inst->alive) run_event(inst, 3, 2);
        }
    }

    // collision events (event 4)
    for (size_t i = 0; i < instances.size(); ++i) {
        Instance* a = instances[i].get();
        if (!a || !a->alive) continue;
        const ObjDef& oa = dw.objects[a->obj];
        auto e4_it = oa.events.find(4);
        if (e4_it == oa.events.end() || e4_it->second.empty()) continue;

        double bb_a[4];
        double ax = to_num(a->vars.count("x") ? a->vars["x"] : Value(0.0));
        double ay = to_num(a->vars.count("y") ? a->vars["y"] : Value(0.0));
        if (!bbox_of(*this, a, ax, ay, bb_a)) continue;

        for (size_t j = 0; j < instances.size(); ++j) {
            if (i == j) continue;
            Instance* b = instances[j].get();
            if (!b || !b->alive) continue;

            double bx = to_num(b->vars.count("x") ? b->vars["x"] : Value(0.0));
            double by = to_num(b->vars.count("y") ? b->vars["y"] : Value(0.0));

            for (const auto& ev_pair : e4_it->second) {
                int target_obj = (int)ev_pair.first;
                if (is_obj(dw, b, target_obj)) {
                    double bb_b[4];
                    if (bbox_of(*this, b, bx, by, bb_b) && overlap(bb_a, bb_b)) {
                        for (int ci : ev_pair.second) {
                            if (ci >= 0 && ci < (int)dw.code.size()) {
                                std::vector<Value> noargs;
                                run_entry(&dw.code[ci], noargs, a, b);
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    if (pending_room >= 0) { int r = pending_room; pending_room = -1; change_room(r); }

    // Pump the audio mixer: render one game-frame's worth of samples so the
    // backend stays fed. Real device output is asynchronous (ALSA); the null
    // backend just advances the deterministic clock.
    if (audio) {
        int speed = (int)(dw.rooms[room_index].speed ? dw.rooms[room_index].speed : 60);
        if (speed <= 0) speed = 60;
        int frames = (int)((double)audio->sample_rate() / (double)speed);
        if (frames > 0) audio->render(frames);
    }
}

Image Runtime::draw() {
    const Room& room = dw.rooms[room_index];
    Image canvas;
    canvas.w = 320; canvas.h = 240;
    canvas.rgba.assign((size_t)canvas.w * canvas.h * 4, 0);
    uint8_t bg[4] = { (uint8_t)(room.bg_color & 0xFF), (uint8_t)((room.bg_color >> 8) & 0xFF),
                      (uint8_t)((room.bg_color >> 16) & 0xFF), 255 };
    for (size_t i = 0; i < canvas.rgba.size(); i += 4) std::memcpy(&canvas.rgba[i], bg, 4);
    screen = &canvas;

    // Viewport camera calculation
    // Viewport camera calculation
    int cam_x = 0, cam_y = 0;
    int follow_obj = -1;
    auto it_vo = global_arrays.find("view_object");
    if (it_vo != global_arrays.end() && !it_vo->second.empty()) follow_obj = (int)to_num(it_vo->second[0]);

    bool following = false;
    if (room.width > 320 || room.height > 240) {
        for (auto& inst : instances) {
            if (inst->alive && (inst->obj == follow_obj || dw.objects[inst->obj].name == "obj_mainchara")) {
                double px = to_num(inst->vars.count("x") ? inst->vars["x"] : Value(0.0));
                double py = to_num(inst->vars.count("y") ? inst->vars["y"] : Value(0.0));
                cam_x = (int)px - 160;
                cam_y = (int)py - 120;
                following = true;
                break;
            }
        }
    }
    if (!following) {
        auto it_vx = global_arrays.find("view_xview");
        auto it_vy = global_arrays.find("view_yview");
        if (it_vx != global_arrays.end() && !it_vx->second.empty()) cam_x = (int)to_num(it_vx->second[0]);
        if (it_vy != global_arrays.end() && !it_vy->second.empty()) cam_y = (int)to_num(it_vy->second[0]);
    }
    cam_x = std::clamp(cam_x, 0, std::max(0, (int)room.width - 320));
    cam_y = std::clamp(cam_y, 0, std::max(0, (int)room.height - 240));

    view_x = cam_x;
    view_y = cam_y;
    if (global_arrays.count("view_xview") && !global_arrays["view_xview"].empty())
        global_arrays["view_xview"][0] = Value((double)cam_x);
    if (global_arrays.count("view_yview") && !global_arrays["view_yview"].empty())
        global_arrays["view_yview"][0] = Value((double)cam_y);

    // Draw room backgrounds
    for (const auto& rb : room.backgrounds) {
        if (!rb.enabled || rb.bgnd < 0 || rb.bgnd >= (int)dw.bgnds.size() || rb.foreground) continue;
        int tp = dw.bgnds[rb.bgnd].tpag;
        if (tp < 0 || tp >= (int)dw.tpags.size()) continue;
        const Tpag& t = dw.tpags[tp];
        if (t.tex < 0 || t.tex >= (int)dw.tex_ptrs.size()) continue;
        const Image& pg = page(t.tex);
        if (pg.w <= 0 || t.sx < 0 || t.sy < 0 || t.sx + t.sw > pg.w || t.sy + t.sh > pg.h) continue;

        int bg_w = t.sw, bg_h = t.sh;
        if (bg_w <= 0 || bg_h <= 0) continue;

        int start_x = rb.x - view_x;
        int start_y = rb.y - view_y;
        if (rb.tile_x) start_x = (start_x % bg_w) - bg_w;
        if (rb.tile_y) start_y = (start_y % bg_h) - bg_h;
        int end_x = rb.tile_x ? canvas.w : start_x + bg_w;
        int end_y = rb.tile_y ? canvas.h : start_y + bg_h;

        for (int by = start_y; by < end_y; by += bg_h) {
            for (int bx = start_x; bx < end_x; bx += bg_w) {
                blit_sub_screen(pg, t.sx, t.sy, bg_w, bg_h, bx, by, 1.0);
            }
        }
    }

    // Tiles (with frustum culling and zero-copy blitting)
    std::vector<const Tile*> ts;
    for (auto& t : room.tiles) {
        if (t.x + t.w <= view_x || t.x >= view_x + canvas.w ||
            t.y + t.h <= view_y || t.y >= view_y + canvas.h) continue;
        ts.push_back(&t);
    }
    std::sort(ts.begin(), ts.end(), [](const Tile* a, const Tile* b){ return a->depth > b->depth; });
    for (const Tile* tl : ts) {
        if (tl->bgnd < 0 || tl->bgnd >= (int)dw.bgnds.size()) continue;
        int tp = dw.bgnds[tl->bgnd].tpag;
        if (tp < 0 || tp >= (int)dw.tpags.size()) continue;
        const Tpag& t = dw.tpags[tp];
        if (t.tex < 0 || t.tex >= (int)dw.tex_ptrs.size()) continue;
        const Image& pg = page(t.tex);
        if (tl->srcx < 0 || tl->srcy < 0 || tl->srcx + tl->w > pg.w || tl->srcy + tl->h > pg.h) continue;
        blit_sub_screen(pg, tl->srcx, tl->srcy, tl->w, tl->h, tl->x - view_x, tl->y - view_y, 1.0);
    }

    // instances by depth
    std::vector<Instance*> order;
    for (auto& i : instances) if (i->alive) order.push_back(i.get());
    std::sort(order.begin(), order.end(), [](Instance* a, Instance* b){
        return to_num(a->vars.count("depth")?a->vars["depth"]:Value(0.0)) >
               to_num(b->vars.count("depth")?b->vars["depth"]:Value(0.0)); });
    for (Instance* inst : order) {
        bool has_draw = false;
        int cur_obj = inst->obj;
        for (int p = 0; p < 16 && cur_obj >= 0 && cur_obj < (int)dw.objects.size(); ++p) {
            auto it = dw.objects[cur_obj].events.find(8);
            if (it != dw.objects[cur_obj].events.end() && it->second.count(0)) {
                has_draw = true;
                break;
            }
            cur_obj = dw.objects[cur_obj].parent;
        }
        if (has_draw) run_event(inst, 8, 0);
        else if (to_num(inst->vars.count("visible")?inst->vars["visible"]:Value(1.0)) != 0)
            draw_sprite_impl(*this, to_num(inst->vars.count("sprite_index")?inst->vars["sprite_index"]:Value(-1.0)),
                             to_num(inst->vars.count("image_index")?inst->vars["image_index"]:Value(0.0)),
                             to_num(inst->vars.count("x")?inst->vars["x"]:Value(0.0)),
                             to_num(inst->vars.count("y")?inst->vars["y"]:Value(0.0)),
                             to_num(inst->vars.count("image_xscale")?inst->vars["image_xscale"]:Value(1.0)),
                             to_num(inst->vars.count("image_yscale")?inst->vars["image_yscale"]:Value(1.0)),
                             to_num(inst->vars.count("image_alpha")?inst->vars["image_alpha"]:Value(1.0)));
    }
    screen = nullptr;
    return canvas;
}

} // namespace gm14
