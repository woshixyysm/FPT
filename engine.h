
#ifndef SIMPLE_SCRIPT_ENGINE_H
#define SIMPLE_SCRIPT_ENGINE_H

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <exception>
#include <stdexcept>
#include <complex>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <regex>
#include <random>
#include <omp.h>
//#include "memory.h"

using namespace std;

#ifdef __has_include
# if __has_include(<boost/multiprecision/cpp_int.hpp>)
#  include <boost/multiprecision/cpp_int.hpp>
#  include <boost/multiprecision/cpp_dec_float.hpp>
#  define HAS_BOOST_MP 1
# endif
#endif

#ifdef HAS_BOOST_MP
using BigInt = boost::multiprecision::cpp_int;
using BigFloat = boost::multiprecision::number<boost::multiprecision::cpp_dec_float<50>>;
#endif

struct VM;
struct Compiler;
struct Expr;
struct Stmt;

struct Token {
    enum class Type {
        L_PAREN, R_PAREN, L_BRACE, R_BRACE, L_BRACKET, R_BRACKET,
        COMMA, DOT, MINUS, PLUS, SEMICOLON, SLASH, STAR, COLON,
        BANG, BANG_EQUAL, EQUAL, EQUAL_EQUAL, GREATER, GREATER_EQUAL, LESS, LESS_EQUAL,
        POWER, SPREAD, ARROW,
        IDENTIFIER, STRING, NUMBER,
        AND, OR, IF, ELSE, TRUE, FALSE, NIL, RETURN, FOR, IN, WHILE, FN, LET, PRINT, MUT,
        INP, INCPORT, AS, SWITCH, CASE, DEFAULT, TRY, CATCH, THROW, BREAK, CONTINUE, MATCH, PUSH, STRUCT, GOTO,
        END_OF_FILE
    } type;
    string lexeme;
    int line;
};

using ExprPtr = shared_ptr<Expr>;
using StmtPtr = shared_ptr<Stmt>;

struct NDArray {
    vector<size_t> dims;
    vector<complex<double>> data;           
    NDArray() = default;
    NDArray(const vector<size_t>& d): dims(d) {
        size_t n=1; for(auto x:d) n*=x;
        data.assign(n, complex<double>(0.0,0.0));
    }
    size_t size() const { size_t n=1; for(auto x:dims) n*=x; return n; }
    
    size_t offset(const vector<size_t>& idx) const {
        size_t off = 0;
        size_t stride = 1;
        for(size_t k=0;k<idx.size();++k){
            off += idx[k] * stride;
            stride *= dims[k];
        }
        return off;
    }
   
    NDArray transpose2d() const {
        if(dims.size()!=2) return *this;
        NDArray out({dims[1], dims[0]});
        size_t r = dims[0], c = dims[1];
        for(size_t i=0;i<r;++i) for(size_t j=0;j<c;++j){
            
            out.data[i*c + j] = data[j*r + i]; 
        }
        return out;
    }
};

enum class ValueKind { NIL, NUMBER, INT, BOOL, STRING, ARRAY, MAP, FUNCTION, NATIVE, MATRIX, NDARRAY, COMPLEX, MODULE
#ifdef HAS_BOOST_MP
, BIGINT, BIGFLOAT
#endif
};

struct Value {
    ValueKind kind = ValueKind::NIL;
    double number_value = 0.0;
    int64_t int_value = 0;
    bool bool_value = false;
    string string_value;
    vector<shared_ptr<Value>> array_value;
    unordered_map<string, shared_ptr<Value>> map_value;
    shared_ptr<struct FunctionObj> fn_value;
    function<shared_ptr<Value>(const vector<shared_ptr<Value>>&)> native_fn;
    shared_ptr<NDArray> ndarray_value;
    complex<double> complex_value = {0.0,0.0};
#ifdef HAS_BOOST_MP
    BigInt bigint_value;
    BigFloat bigfloat_value;
#endif

    Value() = default;
    static shared_ptr<Value> make_nil(){ return make_shared<Value>(); }
    static shared_ptr<Value> make_number(double v){ auto p = make_shared<Value>(); p->kind = ValueKind::NUMBER; p->number_value = v; return p; }
    static shared_ptr<Value> make_int(int64_t v){ auto p = make_shared<Value>(); p->kind = ValueKind::INT; p->int_value = v; return p; }
    static shared_ptr<Value> make_bool(bool b){ auto p = make_shared<Value>(); p->kind = ValueKind::BOOL; p->bool_value = b; return p; }
    static shared_ptr<Value> make_string(const string &s){ auto p = make_shared<Value>(); p->kind = ValueKind::STRING; p->string_value = s; return p; }
    static shared_ptr<Value> make_array(){ auto p = make_shared<Value>(); p->kind = ValueKind::ARRAY; return p; }
    static shared_ptr<Value> make_map(){ auto p = make_shared<Value>(); p->kind = ValueKind::MAP; return p; }
    static shared_ptr<Value> make_function(shared_ptr<struct FunctionObj> f){ auto p = make_shared<Value>(); p->kind = ValueKind::FUNCTION; p->fn_value = f; return p; }
    static shared_ptr<Value> make_native(function<shared_ptr<Value>(const vector<shared_ptr<Value>>&)> fn){ auto p = make_shared<Value>(); p->kind = ValueKind::NATIVE; p->native_fn = fn; return p; }
    static shared_ptr<Value> make_ndarray(const shared_ptr<NDArray>& a){ auto p = make_shared<Value>(); p->kind = ValueKind::NDARRAY; p->ndarray_value = a; return p; }
    static shared_ptr<Value> make_complex(complex<double> c){ auto p = make_shared<Value>(); p->kind = ValueKind::COMPLEX; p->complex_value = c; return p; }
    static shared_ptr<Value> make_module(){ auto p = make_shared<Value>(); p->kind = ValueKind::MODULE; return p; }

    string toString() const {
        switch (kind) {
        case ValueKind::NIL: return "nil";

        case ValueKind::NUMBER: {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.15g", number_value);
            char* dot = strchr(tmp, '.');
            if (dot) {
                char* end = tmp + strlen(tmp) - 1;
                while (end > dot && *end == '0') *end-- = '\0';
                if (end == dot) *end = '\0';
            }
            return std::string(tmp);
        }

        case ValueKind::INT: return std::to_string(int_value);
        case ValueKind::BOOL: return bool_value ? "true" : "false";
        case ValueKind::STRING: return string_value;

        case ValueKind::ARRAY: {
            if (array_value.empty()) return "[]";
            const size_t max_items = 100;
            const size_t n = std::min(array_value.size(), max_items);

            std::string s;
            s.reserve(n * 20);
            s += "[";

            for (size_t i = 0; i < n; ++i) {
                s += array_value[i]->toString();
                if (i + 1 < array_value.size()) s += ", ";
            }

            if (array_value.size() > max_items) {
                s += ", ... (" + std::to_string(array_value.size()) + " items)";
            }
            s += "]";
            return s;
        }

        case ValueKind::MAP: {
            if (map_value.empty()) return "{}";
            const size_t max_items = 50;
            size_t count = 0;

            std::string s;
            s.reserve(max_items * 40);
            s += "{";
            bool first = true;

            for (auto& kv : map_value) {
                if (count++ >= max_items) {
                    s += ", ...}";
                    return s;
                }
                if (!first) s += ", ";
                first = false;
                s += kv.first + ":" + kv.second->toString();
            }
            s += "}";
            return s;
        }

        case ValueKind::FUNCTION: return "<fn>";
        case ValueKind::NATIVE: return "<native>";

        case ValueKind::NDARRAY: {
            std::string s = "ndarray(";
            for (size_t i = 0; i < ndarray_value->dims.size(); ++i) {
                if (i) s += "x";
                s += std::to_string(ndarray_value->dims[i]);
            }
            s += ")";
            return s;
        }

        case ValueKind::COMPLEX: {
            return std::to_string(complex_value.real()) + "+" +
                std::to_string(complex_value.imag()) + "i";
        }

        case ValueKind::MODULE: return "<module>";

#ifdef HAS_BOOST_MP
        case ValueKind::BIGINT: return boost::multiprecision::to_string(bigint_value);
        case ValueKind::BIGFLOAT: return bigfloat_value.convert_to<string>();
#endif
        default: return "<val>";
        }}}; 

struct VarEntry { shared_ptr<Value> val; bool is_mut; VarEntry(shared_ptr<Value> v=nullptr,bool m=true): val(v), is_mut(m){} };

struct Environment : enable_shared_from_this<Environment> {
    unordered_map<string, VarEntry> values;
    shared_ptr<Environment> parent;
    Environment(shared_ptr<Environment> p = nullptr): parent(p){}
    void define(const string &name, shared_ptr<Value> val, bool is_mut=true) { values[name] = VarEntry(val, is_mut); }
    bool existsLocal(const string &name) const { return values.find(name) != values.end(); }
    void assign(const string &name, shared_ptr<Value> val) {
        auto it = values.find(name);
        if(it != values.end()){
            if(!it->second.is_mut) throw runtime_error("assign to immutable '" + name + "'");
            it->second.val = val; return;
        }
        if(parent){ try{ parent->assign(name, val); return; } catch(...){} }
        values[name] = VarEntry(val, true);
    }
    VarEntry* findEntry(const string &name){
        auto it = values.find(name);
        if(it!=values.end()) return &it->second;
        if(parent) return parent->findEntry(name);
        return nullptr;
    }
    shared_ptr<Value> get(const string &name){
        VarEntry* e = findEntry(name);
        if(e) return e->val;
        return nullptr;
    }
};

struct FunctionObj {
    enum Op : uint8_t {
        OP_CONST, OP_LOAD_LOCAL, OP_STORE_LOCAL, OP_LOAD_GLOBAL, OP_STORE_GLOBAL,
        OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW,
        OP_NEG, OP_NOT,
        OP_ARRAY_MAKE, OP_MAP_MAKE,
        OP_GET_INDEX, OP_SET_INDEX, OP_GET_MEMBER,
        OP_CALL, OP_RETURN, OP_POP,
        
        OP_NIL, OP_TRUE, OP_FALSE, OP_GET_LOCAL, OP_SET_LOCAL, OP_GET_GLOBAL, OP_SET_GLOBAL
    };
    struct Instr { Op op; int arg; };
    vector<Instr> code;
    vector<shared_ptr<Value>> consts;
    vector<string> params;
    FunctionObj() = default;
};

struct Compiler {
    shared_ptr<FunctionObj> fn;
    unordered_map<string,int> localIndex;
    
    Compiler(){ fn = make_shared<FunctionObj>(); }
    
    int addConst(shared_ptr<Value> v){ fn->consts.push_back(v); return (int)fn->consts.size()-1; }
    void emit(FunctionObj::Op op, int arg=0){ fn->code.push_back({op,arg}); }
    
    
    void emitByte(uint8_t op){ fn->code.push_back({(FunctionObj::Op)op, 0}); }
    void emitBytes(uint8_t a, uint8_t b){ fn->code.push_back({(FunctionObj::Op)a, b}); }
    void emitConstant(shared_ptr<Value> v){ emit(FunctionObj::OP_CONST, addConst(v)); }
    int resolveLocal(const string& name){ 
        auto it = localIndex.find(name);
        return it != localIndex.end() ? it->second : -1;
    }

    void compileExpr(shared_ptr<Expr> expr);
    void compileStmt(shared_ptr<Stmt> stmt);
};

struct VM {
    shared_ptr<Environment> globals;
    vector<shared_ptr<Value>> stack;
    struct Frame { shared_ptr<FunctionObj> fn; int ip; vector<shared_ptr<Value>> locals; Frame(shared_ptr<FunctionObj> f, int n): fn(f), ip(0), locals(n){} };
    vector<Frame> frames;
    VM(shared_ptr<Environment> g): globals(g) {}
    void push(shared_ptr<Value> v){ stack.push_back(v); }
    shared_ptr<Value> pop(){ if(stack.empty()) return Value::make_nil(); auto v = stack.back(); stack.pop_back(); return v; }
    static double toNumber(const shared_ptr<Value>& v){ if(!v) return 0.0; if(v->kind==ValueKind::NUMBER) return v->number_value; if(v->kind==ValueKind::INT) return (double)v->int_value; return 0.0; }
    static bool isTruthy(const shared_ptr<Value>& v){ if(!v) return false; if(v->kind==ValueKind::NIL) return false; if(v->kind==ValueKind::BOOL) return v->bool_value; if(v->kind==ValueKind::NUMBER) return v->number_value!=0.0; if(v->kind==ValueKind::INT) return v->int_value!=0; return true; }

    shared_ptr<Value> run(shared_ptr<FunctionObj> fn, const vector<shared_ptr<Value>> &args){
        frames.emplace_back(fn, (int)max(fn->params.size(), args.size()));
        for(size_t i=0;i<args.size() && i<frames.back().locals.size(); ++i) frames.back().locals[i] = args[i];
        while(!frames.empty()){
            auto &F = frames.back();
            if(F.ip >= (int)F.fn->code.size()){ frames.pop_back(); continue; }
            auto instr = F.fn->code[F.ip++];
            switch(instr.op){
                case FunctionObj::OP_CONST: push(F.fn->consts[instr.arg]); break;
                case FunctionObj::OP_LOAD_LOCAL: { int idx=instr.arg; if(idx>=0 && idx < (int)F.locals.size()) push(F.locals[idx]); else push(Value::make_nil()); break; }
                case FunctionObj::OP_STORE_LOCAL: { int idx=instr.arg; auto v = pop(); if(idx>=0 && idx < (int)F.locals.size()) F.locals[idx] = v; break; }
                case FunctionObj::OP_LOAD_GLOBAL: { auto namev = F.fn->consts[instr.arg]; string name = (namev->kind==ValueKind::STRING?namev->string_value:namev->toString()); auto gv = globals->get(name); push(gv?gv:Value::make_nil()); break; }
                case FunctionObj::OP_STORE_GLOBAL: { auto namev = F.fn->consts[instr.arg]; string name = (namev->kind==ValueKind::STRING?namev->string_value:namev->toString()); auto val = pop(); globals->assign(name,val); break; }
                case FunctionObj::OP_ADD: { auto b=pop(), a=pop(); if(a->kind==ValueKind::STRING||b->kind==ValueKind::STRING) push(Value::make_string(a->toString()+b->toString())); else if(a->kind==ValueKind::INT&&b->kind==ValueKind::INT) push(Value::make_int(a->int_value+b->int_value)); else push(Value::make_number(toNumber(a)+toNumber(b))); break; }
                case FunctionObj::OP_SUB: { auto b=pop(), a=pop(); if(a->kind==ValueKind::INT&&b->kind==ValueKind::INT) push(Value::make_int(a->int_value-b->int_value)); else push(Value::make_number(toNumber(a)-toNumber(b))); break; }
                case FunctionObj::OP_MUL: { auto b=pop(), a=pop(); if(a->kind==ValueKind::INT&&b->kind==ValueKind::INT) push(Value::make_int(a->int_value*b->int_value)); else push(Value::make_number(toNumber(a)*toNumber(b))); break; }
                case FunctionObj::OP_DIV: { auto b=pop(), a=pop(); push(Value::make_number(toNumber(a)/toNumber(b))); break; }
                case FunctionObj::OP_POW: { auto b=pop(), a=pop(); push(Value::make_number(pow(toNumber(a), toNumber(b)))); break; }
                case FunctionObj::OP_NEG: { auto a=pop(); if(a->kind==ValueKind::INT) push(Value::make_int(-a->int_value)); else push(Value::make_number(-toNumber(a))); break; }
                case FunctionObj::OP_NOT: { auto a=pop(); push(Value::make_bool(!isTruthy(a))); break; }
                case FunctionObj::OP_ARRAY_MAKE: {
                    int n = instr.arg; auto arr = Value::make_array(); arr->array_value.resize(n); for(int i=n-1;i>=0;--i) arr->array_value[i] = pop(); push(arr); break;
                }
                case FunctionObj::OP_MAP_MAKE: {
                    int n = instr.arg; auto mp = Value::make_map();
                    for(int i=0;i<n;++i){ auto keyv = pop(); auto valv = pop(); string key = (keyv->kind==ValueKind::STRING?keyv->string_value:keyv->toString()); mp->map_value[key] = valv; }
                    push(mp); break;
                }
                case FunctionObj::OP_GET_INDEX: {
                    auto idx = pop(); auto obj = pop();
                    if(obj->kind==ValueKind::ARRAY && (idx->kind==ValueKind::INT || idx->kind==ValueKind::NUMBER)){
                        int i = (idx->kind==ValueKind::INT)? (int)idx->int_value : (int)idx->number_value;
                        if(i<0 || i>=(int)obj->array_value.size()) push(Value::make_nil()); else push(obj->array_value[i]);
                    } else if(obj->kind==ValueKind::MAP && idx->kind==ValueKind::STRING){
                        auto it = obj->map_value.find(idx->string_value); if(it!=obj->map_value.end()) push(it->second); else push(Value::make_nil());
                    } else push(Value::make_nil());
                    break;
                }
                case FunctionObj::OP_SET_INDEX: {
                    auto val = pop(); auto idx = pop(); auto obj = pop();
                    if(obj->kind==ValueKind::ARRAY && (idx->kind==ValueKind::INT || idx->kind==ValueKind::NUMBER)){
                        int i = (idx->kind==ValueKind::INT)? (int)idx->int_value : (int)idx->number_value;
                        if(i>=0 && i < (int)obj->array_value.size()) obj->array_value[i] = val;
                    }
                    push(val);
                    break;
                }
                case FunctionObj::OP_GET_MEMBER: {
                    auto namev = F.fn->consts[instr.arg]; string mname = (namev->kind==ValueKind::STRING?namev->string_value:namev->toString());
                    auto obj = pop();
                    if(obj->kind==ValueKind::MAP){
                        auto it = obj->map_value.find(mname);
                        if(it!=obj->map_value.end()) push(it->second); else push(Value::make_nil());
                    } else push(Value::make_nil());
                    break;
                }
                case FunctionObj::OP_CALL: {
                    int argc = instr.arg; vector<shared_ptr<Value>> args(argc); for(int i=argc-1;i>=0;--i) args[i] = pop(); auto callee = pop();
                    if(!callee){ push(Value::make_nil()); break; }
                    if(callee->kind==ValueKind::NATIVE){ try{ push(callee->native_fn(args)); } catch(...) { push(Value::make_nil()); } }
                    else if(callee->kind==ValueKind::FUNCTION && callee->fn_value){
                        auto nested = callee->fn_value;
                        frames.emplace_back(nested, (int)max(nested->params.size(), (size_t)argc));
                        for(int i=0;i<argc && i<(int)frames.back().locals.size(); ++i) frames.back().locals[i] = args[i];
                    } else push(Value::make_nil());
                    break;
                }
                case FunctionObj::OP_RETURN: {
                    auto rv = pop();
                    frames.pop_back();
                    if(frames.empty()) return rv;
                    else push(rv);
                    break;
                }
                case FunctionObj::OP_POP: pop(); break;
                default: break;
            }
        }
        return Value::make_nil();
    }
};

struct ThrowException { shared_ptr<Value> v; ThrowException(shared_ptr<Value> vv): v(vv){} };
struct BreakException {};
struct ContinueException {};
struct GotoException { string label; GotoException(string l): label(std::move(l)){} };

#endif