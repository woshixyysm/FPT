#include <cmath>
#include <limits>
#include <random> 
#include <algorithm> 

#if defined(__unix__) || defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

#if defined(__has_include)
#  if __has_include(<mpi.h>)
#    include <mpi.h>
#    define HAS_MPI 1
#  endif
#endif
#include "engine.h"
#include "memory.h"
using std::move;

struct Expr { virtual ~Expr()=default; };
struct Stmt { virtual ~Stmt()=default; };

struct LiteralExpr : Expr { shared_ptr<Value> val; LiteralExpr(shared_ptr<Value> v): val(v){} };
struct VariableExpr : Expr { string name; VariableExpr(string n): name(move(n)){} };
struct AssignExpr : Expr { shared_ptr<Expr> target; shared_ptr<Expr> value; AssignExpr(shared_ptr<Expr> t, shared_ptr<Expr> v): target(t), value(v){} };
struct BinaryExpr : Expr { string op; shared_ptr<Expr> left,right; BinaryExpr(shared_ptr<Expr> l, string o, shared_ptr<Expr> r): left(l), op(move(o)), right(r){} };
struct UnaryExpr : Expr { string op; shared_ptr<Expr> right; UnaryExpr(string o, shared_ptr<Expr> r): op(move(o)), right(r){} };
struct CallExpr : Expr { shared_ptr<Expr> callee; vector<shared_ptr<Expr>> args; CallExpr(shared_ptr<Expr> c, vector<shared_ptr<Expr>> a): callee(c), args(move(a)){} };
struct IndexExpr : Expr { shared_ptr<Expr> object, index; IndexExpr(shared_ptr<Expr> o, shared_ptr<Expr> i): object(o), index(i){} };
struct SliceExpr : Expr {
    shared_ptr<Expr> object;
    shared_ptr<Expr> start, stop, step;
    SliceExpr(shared_ptr<Expr> o, shared_ptr<Expr> s, shared_ptr<Expr> e, shared_ptr<Expr> st): object(o), start(s), stop(e), step(st) {}
};
struct ArrayExpr : Expr { vector<shared_ptr<Expr>> elements; ArrayExpr(vector<shared_ptr<Expr>> e): elements(move(e)){} };
struct MapExpr : Expr { vector<pair<string, shared_ptr<Expr>>> pairs; MapExpr(vector<pair<string, shared_ptr<Expr>>> p): pairs(move(p)){} };
struct MemberExpr : Expr { shared_ptr<Expr> object; string name; MemberExpr(shared_ptr<Expr> o, string n): object(o), name(move(n)){} };
struct SpreadExpr : Expr { shared_ptr<Expr> inner; SpreadExpr(shared_ptr<Expr> i): inner(i){} };
struct LambdaExpr : Expr { vector<string> params; vector<shared_ptr<Stmt>> body; LambdaExpr(vector<string> p, vector<shared_ptr<Stmt>> b): params(move(p)), body(move(b)){} };

struct ExprStmt : Stmt { shared_ptr<Expr> expr; ExprStmt(shared_ptr<Expr> e): expr(e){} };
struct VarStmt : Stmt { shared_ptr<Expr> pattern; shared_ptr<Expr> initializer; bool is_mut=false; VarStmt(shared_ptr<Expr> p, shared_ptr<Expr> i, bool m=false): pattern(p), initializer(i), is_mut(m){} };
struct BlockStmt : Stmt { vector<shared_ptr<Stmt>> statements; BlockStmt(vector<shared_ptr<Stmt>> s): statements(move(s)){} };
struct IfStmt : Stmt { shared_ptr<Expr> cond; shared_ptr<Stmt> thenB, elseB; IfStmt(shared_ptr<Expr> c, shared_ptr<Stmt> t, shared_ptr<Stmt> e): cond(c), thenB(t), elseB(e){} };
struct WhileStmt : Stmt { shared_ptr<Expr> cond; shared_ptr<Stmt> body; WhileStmt(shared_ptr<Expr> c, shared_ptr<Stmt> b): cond(c), body(b){} };
struct ForInStmt : Stmt { string var; shared_ptr<Expr> iterable; shared_ptr<Stmt> body; ForInStmt(string v, shared_ptr<Expr> it, shared_ptr<Stmt> b): var(move(v)), iterable(it), body(b){} };
struct ReturnStmt : Stmt { shared_ptr<Expr> value; ReturnStmt(shared_ptr<Expr> v): value(v){} };
struct FunctionStmt : Stmt { string name; vector<string> params; vector<shared_ptr<Stmt>> body; FunctionStmt(string n, vector<string> p, vector<shared_ptr<Stmt>> b): name(n), params(move(p)), body(move(b)){} };
struct LabelStmt : Stmt { string name; LabelStmt(string n): name(move(n)){} };
struct GotoStmt : Stmt { string name; GotoStmt(string n): name(move(n)){} };
struct PushStmt : Stmt { shared_ptr<Expr> target; vector<shared_ptr<Expr>> vals; PushStmt(shared_ptr<Expr> t, vector<shared_ptr<Expr>> v): target(t), vals(move(v)){} };
struct ThrowStmt : Stmt { shared_ptr<Expr> expr; ThrowStmt(shared_ptr<Expr> e): expr(e){} };
struct TryCatchStmt : Stmt { shared_ptr<Stmt> tryBlock; string exName; shared_ptr<Stmt> catchBlock; TryCatchStmt(shared_ptr<Stmt> t, string n, shared_ptr<Stmt> c): tryBlock(t), exName(n), catchBlock(c){} };
struct MatchArm { shared_ptr<Expr> pattern; shared_ptr<Expr> body; };
struct MatchExpr : Expr { shared_ptr<Expr> discr; vector<MatchArm> arms; shared_ptr<Expr> defaultArm; MatchExpr(shared_ptr<Expr> d): discr(d){} };
struct PrintStmt : Stmt { vector<shared_ptr<Expr>> args; PrintStmt(vector<shared_ptr<Expr>> a) : args(move(a)) {} };


void Compiler::compileExpr(shared_ptr<Expr> expr) {
    if(auto lit = dynamic_pointer_cast<LiteralExpr>(expr)){
        if(lit->val->kind == ValueKind::NIL) emitByte(FunctionObj::OP_NIL);
        else if(lit->val->kind == ValueKind::BOOL){
            emitByte(lit->val->bool_value ? FunctionObj::OP_TRUE : FunctionObj::OP_FALSE);
        }
        else emitConstant(lit->val);
    }
    else if(auto var = dynamic_pointer_cast<VariableExpr>(expr)){
        int arg = resolveLocal(var->name);
        if(arg != -1){
            emitBytes(FunctionObj::OP_GET_LOCAL, (uint8_t)arg);
        } else {
            size_t constant = addConst(Value::make_string(var->name));
            emitBytes(FunctionObj::OP_GET_GLOBAL, (uint8_t)constant);
        }
    }
    else if(auto bin = dynamic_pointer_cast<BinaryExpr>(expr)){
        compileExpr(bin->left);
        compileExpr(bin->right);
        if(bin->op == "+") emitByte(FunctionObj::OP_ADD);
        else if(bin->op == "-") emitByte(FunctionObj::OP_SUB);
        else if(bin->op == "*") emitByte(FunctionObj::OP_MUL);
        else if(bin->op == "/") emitByte(FunctionObj::OP_DIV);
    }
    else if(auto un = dynamic_pointer_cast<UnaryExpr>(expr)){
        compileExpr(un->right);
        if(un->op == "-") emitByte(FunctionObj::OP_NEG);
        else if(un->op == "!") emitByte(FunctionObj::OP_NOT);
    }
    else if(auto call = dynamic_pointer_cast<CallExpr>(expr)){
        compileExpr(call->callee);
        for(auto &arg : call->args){
            compileExpr(arg);
        }
        emit(FunctionObj::OP_CALL, (int)call->args.size());
    }
    else if(auto arr = dynamic_pointer_cast<ArrayExpr>(expr)){
        for(auto &el : arr->elements){
            compileExpr(el);
        }
        emit(FunctionObj::OP_ARRAY_MAKE, (int)arr->elements.size());
    }
    else if(auto mp = dynamic_pointer_cast<MapExpr>(expr)){
        for(auto &p : mp->pairs){
            emitConstant(Value::make_string(p.first));
            compileExpr(p.second);
        }
        emit(FunctionObj::OP_MAP_MAKE, (int)mp->pairs.size());
    }
    else if(auto idx = dynamic_pointer_cast<IndexExpr>(expr)){
        compileExpr(idx->object);
        compileExpr(idx->index);
        emitByte(FunctionObj::OP_GET_INDEX);
    }
    else if(auto mem = dynamic_pointer_cast<MemberExpr>(expr)){
        compileExpr(mem->object);
        size_t nameConst = addConst(Value::make_string(mem->name));
        emit(FunctionObj::OP_GET_MEMBER, (int)nameConst);
    }
    else if(auto asgn = dynamic_pointer_cast<AssignExpr>(expr)){
        compileExpr(asgn->value);
        if(auto var = dynamic_pointer_cast<VariableExpr>(asgn->target)){
            int local = resolveLocal(var->name);
            if(local != -1){
                emitBytes(FunctionObj::OP_SET_LOCAL, (uint8_t)local);
            } else {
                size_t global = addConst(Value::make_string(var->name));
                emitBytes(FunctionObj::OP_SET_GLOBAL, (uint8_t)global);
            }
        }
        else if(auto idx = dynamic_pointer_cast<IndexExpr>(asgn->target)){
            compileExpr(idx->object);
            compileExpr(idx->index);
            emitByte(FunctionObj::OP_SET_INDEX);
        }
    }
    else if(auto lam = dynamic_pointer_cast<LambdaExpr>(expr)){
        Compiler c;
        for(size_t i=0;i<lam->params.size();++i) c.localIndex[lam->params[i]] = (int)i;
        c.fn->params = lam->params;
        for(auto &st: lam->body) c.compileStmt(st);
        c.emit(FunctionObj::OP_CONST, c.addConst(Value::make_nil()));
        c.emit(FunctionObj::OP_RETURN);
        emitConstant(Value::make_function(c.fn));
    }
    else if(auto match = dynamic_pointer_cast<MatchExpr>(expr)){
        compileExpr(match->discr);
        for(auto &arm : match->arms){
            compileExpr(arm.pattern);
            compileExpr(arm.body);
        }
        if(match->defaultArm) compileExpr(match->defaultArm);
    }
}

void Compiler::compileStmt(shared_ptr<Stmt> stmt) {
    if(auto es = dynamic_pointer_cast<ExprStmt>(stmt)){
        compileExpr(es->expr);
        emitByte(FunctionObj::OP_POP);
    }
    else if(auto vs = dynamic_pointer_cast<VarStmt>(stmt)){
        if(vs->initializer) compileExpr(vs->initializer);
        else emitByte(FunctionObj::OP_NIL);
        
        if(auto var = dynamic_pointer_cast<VariableExpr>(vs->pattern)){
            int local = resolveLocal(var->name);
            if(local != -1){
                emitBytes(FunctionObj::OP_SET_LOCAL, (uint8_t)local);
            } else {
                size_t global = addConst(Value::make_string(var->name));
                emitBytes(FunctionObj::OP_SET_GLOBAL, (uint8_t)global);
            }
        }
        emitByte(FunctionObj::OP_POP);
    }
    else if(auto fs = dynamic_pointer_cast<FunctionStmt>(stmt)){
        Compiler c;
        for(size_t i=0;i<fs->params.size();++i) c.localIndex[fs->params[i]] = (int)i;
        c.fn->params = fs->params;
        for(auto &st: fs->body) c.compileStmt(st);
        c.emit(FunctionObj::OP_CONST, c.addConst(Value::make_nil()));
        c.emit(FunctionObj::OP_RETURN);
        emitConstant(Value::make_function(c.fn));
        size_t nameConst = addConst(Value::make_string(fs->name));
        emitBytes(FunctionObj::OP_SET_GLOBAL, (uint8_t)nameConst);
    }
    else if(auto is = dynamic_pointer_cast<IfStmt>(stmt)){
        compileExpr(is->cond);
        
        compileStmt(is->thenB);
        if(is->elseB) compileStmt(is->elseB);
    }
    else if(auto ws = dynamic_pointer_cast<WhileStmt>(stmt)){
        compileExpr(ws->cond);
        compileStmt(ws->body);
    }
    else if(auto fis = dynamic_pointer_cast<ForInStmt>(stmt)){
        
        compileExpr(fis->iterable);
        compileStmt(fis->body);
    }
    else if(auto bs = dynamic_pointer_cast<BlockStmt>(stmt)){
        for(auto &s : bs->statements){
            compileStmt(s);
        }
    }
    else if(auto rs = dynamic_pointer_cast<ReturnStmt>(stmt)){
        if(rs->value) compileExpr(rs->value);
        else emitByte(FunctionObj::OP_NIL);
        emitByte(FunctionObj::OP_RETURN);
    }
    else if(auto ts = dynamic_pointer_cast<TryCatchStmt>(stmt)){
        compileStmt(ts->tryBlock);
        if(ts->catchBlock) compileStmt(ts->catchBlock);
    }
    else if(auto ths = dynamic_pointer_cast<ThrowStmt>(stmt)){
        compileExpr(ths->expr);
    }
    else if(auto ps = dynamic_pointer_cast<PushStmt>(stmt)){
        compileExpr(ps->target);
        for(auto &v : ps->vals){
            compileExpr(v);
        }
    }
    else if(auto gs = dynamic_pointer_cast<GotoStmt>(stmt)){
        
    }
    else if(auto ls = dynamic_pointer_cast<LabelStmt>(stmt)){
        
    }
}


struct Scanner {
    string src; size_t start=0, current=0; int line=1;
    vector<Token> tokens;
    unordered_map<string, Token::Type> keywords;

    Scanner(const string &s): src(s) {
        keywords = {
            {"and", Token::Type::AND}, {"or", Token::Type::OR}, {"if", Token::Type::IF}, {"else", Token::Type::ELSE},
            {"true", Token::Type::TRUE}, {"false", Token::Type::FALSE}, {"nil", Token::Type::NIL},
            {"return", Token::Type::RETURN}, {"for", Token::Type::FOR}, {"in", Token::Type::IN}, {"while", Token::Type::WHILE},
            {"fn", Token::Type::FN}, {"let", Token::Type::LET}, {"print", Token::Type::PRINT}, {"mut", Token::Type::MUT},
            {"inp", Token::Type::INP}, {"incport", Token::Type::INCPORT}, {"as", Token::Type::AS},
            {"switch", Token::Type::SWITCH}, {"case", Token::Type::CASE}, {"default", Token::Type::DEFAULT},
            {"try", Token::Type::TRY}, {"catch", Token::Type::CATCH}, {"throw", Token::Type::THROW},
            {"break", Token::Type::BREAK}, {"continue", Token::Type::CONTINUE}, {"match", Token::Type::MATCH},
            {"push", Token::Type::PUSH}, {"struct", Token::Type::STRUCT}, {"goto", Token::Type::GOTO}
        };
    }

    bool isAtEnd(){ return current >= src.size(); }
    char advance(){ return src[current++]; }
    char peek(){ return isAtEnd() ? '\0' : src[current]; }
    char peekNext(){ return (current+1 >= src.size()) ? '\0' : src[current+1]; }
    void add(Token::Type t){ tokens.push_back({t, src.substr(start, current-start), line}); }
    void add(Token::Type t, const string &lex){ tokens.push_back({t, lex, line}); }

    bool match(char c){ if(isAtEnd()) return false; if(src[current] != c) return false; ++current; return true; }

    void scanStringDouble(){
        while(peek()!='"' && !isAtEnd()){ if(peek()=='\n') ++line; advance(); }
        if(isAtEnd()){ add(Token::Type::STRING, src.substr(start+1, current-start-1)); return; }
        advance();
        add(Token::Type::STRING, src.substr(start+1, current-start-2+1));
    }
    void scanStringSingle(){
        while(peek()!='\'' && !isAtEnd()){ if(peek()=='\n') ++line; advance(); }
        if(isAtEnd()){ add(Token::Type::STRING, src.substr(start+1, current-start-1)); return; }
        advance();
        add(Token::Type::STRING, src.substr(start+1, current-start-2+1));
    }
    void scanNumber(){
        bool dot=false;
        while(isdigit(peek()) || peek()=='.'){
            if(peek()=='.'){ if(dot) break; dot=true; }
            advance();
        }
        add(Token::Type::NUMBER, src.substr(start, current-start));
    }
    void scanIdentifier(){
        while(isalnum(peek()) || peek()=='_') advance();
        string txt = src.substr(start, current-start);
        auto it = keywords.find(txt);
        if(it!=keywords.end()) add(it->second, txt);
        else add(Token::Type::IDENTIFIER, txt);
    }

    vector<Token> scanTokens(){
        while(!isAtEnd()){
            start = current;
            char c = advance();
            switch(c){
                case '(' : add(Token::Type::L_PAREN); break;
                case ')' : add(Token::Type::R_PAREN); break;
                case '{' : add(Token::Type::L_BRACE); break;
                case '}' : add(Token::Type::R_BRACE); break;
                case '[' : add(Token::Type::L_BRACKET); break;
                case ']' : add(Token::Type::R_BRACKET); break;
                case ',' : add(Token::Type::COMMA); break;
                case '.' :
                    if(peek()=='.' && peekNext()=='.'){ advance(); advance(); add(Token::Type::SPREAD); }
                    else add(Token::Type::DOT);
                    break;
                case '-' : add(Token::Type::MINUS); break;
                case '+' : add(Token::Type::PLUS); break;
                case ';' : add(Token::Type::SEMICOLON); break;
                case '*' : if(match('*')) add(Token::Type::POWER); else add(Token::Type::STAR); break;
                case ':' : add(Token::Type::COLON); break;
                case '!' : if(match('=')) add(Token::Type::BANG_EQUAL); else add(Token::Type::BANG); break;
                case '=' : {
                    if(peek() == '>'){ advance(); add(Token::Type::ARROW); break; }
                    if(match('=')) add(Token::Type::EQUAL_EQUAL); else add(Token::Type::EQUAL);
                    break;
                }
                case '<' : if(match('=')) add(Token::Type::LESS_EQUAL); else add(Token::Type::LESS); break;
                case '>' : if(match('=')) add(Token::Type::GREATER_EQUAL); else add(Token::Type::GREATER); break;
                case '/' : if(match('/')) { while(peek()!='\n' && !isAtEnd()) advance(); } else add(Token::Type::SLASH); break;
                case ' ':
                case '\r':
                case '\t': break;
                case '\n': ++line; break;
                case '"' : scanStringDouble(); break;
                case '\'' : scanStringSingle(); break;
                default:
                    if(isdigit(c)){ current--; scanNumber(); }
                    else if(isalpha(c) || c=='_'){ current--; scanIdentifier(); }
                    else { /* skip unknown */ }
                    break;
            }
        }
        add(Token::Type::END_OF_FILE);
        return tokens;
    }
};



struct Parser {
    vector<Token> tokens; int current=0;
    Parser(vector<Token> t): tokens(move(t)){}
    bool isAtEnd(){ return tokens[current].type == Token::Type::END_OF_FILE; }
    Token peek(){ return tokens[current]; }
    Token previous(){ return tokens[current-1]; }
    Token advance(){ if(!isAtEnd()) ++current; return previous(); }
    bool check(Token::Type t){ if(isAtEnd()) return false; return peek().type == t; }
    bool match(initializer_list<Token::Type> ts){ for(auto &t: ts) if(check(t)){ advance(); return true; } return false; }
    Token consume(Token::Type t, const string &msg){ if(check(t)) return advance(); return Token{t,msg,peek().line}; }

    vector<shared_ptr<Stmt>> parse(){
        vector<shared_ptr<Stmt>> out;
        while(!isAtEnd()){
            auto s = declaration();
            if(s) out.push_back(s);
            else advance();
        }
        return out;
    }

    shared_ptr<Stmt> declaration(){
        if(match({Token::Type::FN})) return functionDecl();
        if(match({Token::Type::LET})){
            bool is_mut=false; if(match({Token::Type::MUT})) is_mut=true;
            auto pat = pattern();
            shared_ptr<Expr> init=nullptr;
            if(match({Token::Type::EQUAL})) init = expression();
            if(check(Token::Type::SEMICOLON)) advance();
            return make_shared<VarStmt>(pat, init, is_mut);
        }
        
        if(check(Token::Type::IDENTIFIER) && tokens[current+1].type == Token::Type::COLON){
            string lbl = tokens[current].lexeme;
            advance();
            advance();
            return make_shared<LabelStmt>(lbl);
        }
        if(match({Token::Type::INP})){ Token p = consume(Token::Type::STRING, "expect path"); if(check(Token::Type::SEMICOLON)) advance(); auto call = make_shared<CallExpr>(make_shared<VariableExpr>("__builtin_inp"), vector<shared_ptr<Expr>>{ make_shared<LiteralExpr>(Value::make_string(p.lexeme)) }); return make_shared<ExprStmt>(call); }
        if(match({Token::Type::INCPORT})){
            Token p = consume(Token::Type::STRING, "expect path"); string alias;
            if(match({Token::Type::AS})){ Token a = consume(Token::Type::IDENTIFIER,"expect alias"); alias = a.lexeme; }
            if(check(Token::Type::SEMICOLON)) advance();
            vector<shared_ptr<Expr>> args; args.push_back(make_shared<LiteralExpr>(Value::make_string(p.lexeme)));
            if(!alias.empty()) args.push_back(make_shared<LiteralExpr>(Value::make_string(alias)));
            auto call = make_shared<CallExpr>(make_shared<VariableExpr>("__builtin_incport"), args);
            return make_shared<ExprStmt>(call);
        }
        return statement();
    }

    shared_ptr<Stmt> functionDecl(){
        Token name = consume(Token::Type::IDENTIFIER,"expect fn name");
        consume(Token::Type::L_PAREN,"expect (");
        vector<string> params;
        if(!check(Token::Type::R_PAREN)){
            do { Token p = consume(Token::Type::IDENTIFIER,"expect param"); params.push_back(p.lexeme); } while(match({Token::Type::COMMA}));
        }
        consume(Token::Type::R_PAREN,"expect )");
        if(match({Token::Type::MINUS})) match({Token::Type::GREATER});
        consume(Token::Type::L_BRACE,"expect {");
        auto body = block();
        return make_shared<FunctionStmt>(name.lexeme, params, body);
    }

    shared_ptr<Stmt> statement(){
        if(match({Token::Type::L_BRACE})){ auto b = block(); return make_shared<BlockStmt>(b); }
        if(match({Token::Type::IF})){ consume(Token::Type::L_PAREN,"("); auto cond = expression(); consume(Token::Type::R_PAREN,")"); auto thenB = statement(); shared_ptr<Stmt> elseB=nullptr; if(match({Token::Type::ELSE})) elseB = statement(); return make_shared<IfStmt>(cond, thenB, elseB); }
        if(match({Token::Type::WHILE})){ consume(Token::Type::L_PAREN,"("); auto cond = expression(); consume(Token::Type::R_PAREN,")"); auto body = statement(); return make_shared<WhileStmt>(cond, body); }
        if(match({Token::Type::FOR})){ Token var = consume(Token::Type::IDENTIFIER,"for var"); consume(Token::Type::IN,"expect in"); auto iterable = expression(); auto body = statement(); return make_shared<ForInStmt>(var.lexeme, iterable, body); }
        if(match({Token::Type::RETURN})){ shared_ptr<Expr> v=nullptr; if(!check(Token::Type::SEMICOLON)) v=expression(); if(check(Token::Type::SEMICOLON)) advance(); return make_shared<ReturnStmt>(v); }
        if(match({Token::Type::TRY})){ auto tb = statement(); string exName; shared_ptr<Stmt> cb = nullptr; if(match({Token::Type::CATCH})){ consume(Token::Type::L_PAREN,"("); Token id = consume(Token::Type::IDENTIFIER,"expect id"); exName = id.lexeme; consume(Token::Type::R_PAREN,")"); cb = statement(); } return make_shared<TryCatchStmt>(tb, exName, cb); }
        if(match({Token::Type::THROW})){ auto e = expression(); if(check(Token::Type::SEMICOLON)) advance(); return make_shared<ThrowStmt>(e); }
        if(match({Token::Type::PUSH})){
            auto tgt = expression();
            vector<shared_ptr<Expr>> vals;
            while(match({Token::Type::COMMA})){
                if(match({Token::Type::SPREAD})){ auto e = expression(); vals.push_back(make_shared<SpreadExpr>(e)); }
                else vals.push_back(expression());
            }
            if(check(Token::Type::SEMICOLON)) advance();
            return make_shared<PushStmt>(tgt, vals);
        }
        if(match({Token::Type::GOTO})){
            Token id = consume(Token::Type::IDENTIFIER,"expect label"); if(check(Token::Type::SEMICOLON)) advance(); return make_shared<GotoStmt>(id.lexeme);
        }
        if (match({ Token::Type::PRINT })) {
            vector<shared_ptr<Expr>> args;
            if (!check(Token::Type::SEMICOLON) && !isAtEnd()) {
                do {
                    args.push_back(expression());
                } while (match({ Token::Type::COMMA }));
            }
            if (check(Token::Type::SEMICOLON)) advance();
            auto call = make_shared<CallExpr>(
                make_shared<VariableExpr>("__builtin_print"),
                args
            );
            return make_shared<PrintStmt>(args);
        }
        auto expr = expression();
        if(match({Token::Type::EQUAL})){ auto val = expression(); if(check(Token::Type::SEMICOLON)) advance(); return make_shared<ExprStmt>( make_shared<AssignExpr>(expr, val) ); }
        if(check(Token::Type::SEMICOLON)) advance();
        return make_shared<ExprStmt>(expr);
    }

    vector<shared_ptr<Stmt>> block(){
        vector<shared_ptr<Stmt>> stmts;
        while(!check(Token::Type::R_BRACE) && !isAtEnd()){
            auto d = declaration();
            if(d) stmts.push_back(d);
            else advance();
        }
        consume(Token::Type::R_BRACE,"expect }");
        return stmts;
    }

    
    shared_ptr<Expr> pattern(){
        if(match({Token::Type::IDENTIFIER})) return make_shared<VariableExpr>(previous().lexeme);
        if(match({Token::Type::L_BRACKET})){
            vector<shared_ptr<Expr>> elems;
            if(!check(Token::Type::R_BRACKET)){
                do {
                    if(match({Token::Type::SPREAD})){ auto e = pattern(); elems.push_back(make_shared<SpreadExpr>(e)); }
                    else elems.push_back(pattern());
                } while(match({Token::Type::COMMA}));
            }
            consume(Token::Type::R_BRACKET,"expect ]");
            return make_shared<ArrayExpr>(elems);
        }
        if(match({Token::Type::L_BRACE})){
            vector<pair<string, shared_ptr<Expr>>> pairs;
            if(!check(Token::Type::R_BRACE)){
                do {
                    string key;
                    if(match({Token::Type::STRING})) key = previous().lexeme;
                    else if(match({Token::Type::IDENTIFIER})) key = previous().lexeme;
                    else key = "";
                    consume(Token::Type::COLON,"expect :");
                    auto val = pattern();
                    pairs.emplace_back(key, val);
                } while(match({Token::Type::COMMA}));
            }
            consume(Token::Type::R_BRACE,"expect }");
            return make_shared<MapExpr>(pairs);
        }
        return primary();
    }

    
    shared_ptr<Expr> expression(){ return logic_or(); }
    shared_ptr<Expr> logic_or(){ auto e = logic_and(); while(match({Token::Type::OR})){ auto r = logic_and(); e = make_shared<BinaryExpr>(e,"or",r);} return e; }
    shared_ptr<Expr> logic_and(){ auto e = equality(); while(match({Token::Type::AND})){ auto r = equality(); e = make_shared<BinaryExpr>(e,"and",r);} return e; }
    shared_ptr<Expr> equality(){ auto e = comparison(); while(match({Token::Type::BANG_EQUAL, Token::Type::EQUAL_EQUAL})){ Token t = previous(); e = make_shared<BinaryExpr>(e,t.lexeme,comparison()); } return e; }
    shared_ptr<Expr> comparison(){ auto e = term(); while(match({Token::Type::GREATER, Token::Type::GREATER_EQUAL, Token::Type::LESS, Token::Type::LESS_EQUAL})){ Token t = previous(); e = make_shared<BinaryExpr>(e,t.lexeme,term()); } return e; }
    shared_ptr<Expr> term(){ auto e = factor(); while(match({Token::Type::PLUS, Token::Type::MINUS})){ Token t = previous(); e = make_shared<BinaryExpr>(e,t.lexeme,factor()); } return e; }
    shared_ptr<Expr> factor(){ auto e = power(); while(match({Token::Type::STAR, Token::Type::SLASH})){ Token t = previous(); e = make_shared<BinaryExpr>(e,t.lexeme,power()); } return e; }
    shared_ptr<Expr> power(){ auto e = unary(); if(match({Token::Type::POWER})){ auto r = power(); e = make_shared<BinaryExpr>(e,"**",r);} return e; }
    shared_ptr<Expr> unary(){ if(match({Token::Type::BANG, Token::Type::MINUS})){ Token t = previous(); return make_shared<UnaryExpr>(t.lexeme, unary()); } return call(); }

    shared_ptr<Expr> call(){
        auto expr = primary();
        while(true){
            if(match({Token::Type::L_PAREN})){
                vector<shared_ptr<Expr>> args;
                if(!check(Token::Type::R_PAREN)){
                    do {
                        if(match({Token::Type::SPREAD})){ auto e = expression(); args.push_back(make_shared<SpreadExpr>(e)); }
                        else args.push_back(expression());
                    } while(match({Token::Type::COMMA}));
                }
                consume(Token::Type::R_PAREN,"expect )");
                expr = make_shared<CallExpr>(expr, args);
            } else if(match({Token::Type::L_BRACKET})){
                
                shared_ptr<Expr> start = nullptr;
                if(!check(Token::Type::COLON) && !check(Token::Type::R_BRACKET)){
                    start = expression();
                }
                if(match({Token::Type::COLON})){
                    
                    shared_ptr<Expr> stop = nullptr;
                    shared_ptr<Expr> step = nullptr;
                    if(!check(Token::Type::COLON) && !check(Token::Type::R_BRACKET)) stop = expression();
                    if(match({Token::Type::COLON})){
                        if(!check(Token::Type::R_BRACKET)) step = expression();
                    }
                    consume(Token::Type::R_BRACKET, "expect ]");
                    expr = make_shared<SliceExpr>(expr, start, stop, step);
                } else {
                    
                    auto idx = start ? start : expression();
                    consume(Token::Type::R_BRACKET, "expect ]");
                    expr = make_shared<IndexExpr>(expr, idx);
                }
            } else if(match({Token::Type::DOT})){
                Token id = consume(Token::Type::IDENTIFIER,"expect member"); expr = make_shared<MemberExpr>(expr, id.lexeme);
            } else break;
        }
        return expr;
    }

    shared_ptr<Expr> primary(){
        if(match({Token::Type::FALSE})) return make_shared<LiteralExpr>(Value::make_bool(false));
        if(match({Token::Type::TRUE})) return make_shared<LiteralExpr>(Value::make_bool(true));
        if(match({Token::Type::NIL})) return make_shared<LiteralExpr>(Value::make_nil());
        if(match({Token::Type::NUMBER})){
            string lex = previous().lexeme;
            if(lex.find('.')!=string::npos) return make_shared<LiteralExpr>(Value::make_number(stod(lex)));
            try{ return make_shared<LiteralExpr>(Value::make_int(stoll(lex))); } catch(...) { return make_shared<LiteralExpr>(Value::make_number(stod(lex))); }
        }
        if(match({Token::Type::STRING})) return make_shared<LiteralExpr>(Value::make_string(previous().lexeme));
        if(match({Token::Type::IDENTIFIER})) return make_shared<VariableExpr>(previous().lexeme);
        if(match({Token::Type::L_BRACKET})){
            vector<shared_ptr<Expr>> elems;
            if(!check(Token::Type::R_BRACKET)){
                do {
                    if(match({Token::Type::SPREAD})){ auto e = expression(); elems.push_back(make_shared<SpreadExpr>(e)); }
                    else elems.push_back(expression());
                } while(match({Token::Type::COMMA}));
            }
            consume(Token::Type::R_BRACKET,"expect ]");
            return make_shared<ArrayExpr>(elems);
        }
        if(match({Token::Type::L_BRACE})){
            vector<pair<string, shared_ptr<Expr>>> pairs;
            if(!check(Token::Type::R_BRACE)){
                do {
                    string key;
                    if(match({Token::Type::STRING})) key = previous().lexeme;
                    else if(match({Token::Type::IDENTIFIER})) key = previous().lexeme;
                    else key = "";
                    consume(Token::Type::COLON,"expect :");
                    auto val = expression();
                    pairs.emplace_back(key, val);
                } while(match({Token::Type::COMMA}));
            }
            consume(Token::Type::R_BRACE,"expect }");
            return make_shared<MapExpr>(pairs);
        }
        if(match({Token::Type::FN})){
            consume(Token::Type::L_PAREN,"(");
            vector<string> params;
            if(!check(Token::Type::R_PAREN)){
                do { Token p = consume(Token::Type::IDENTIFIER,"param"); params.push_back(p.lexeme); } while(match({Token::Type::COMMA}));
            }
            consume(Token::Type::R_PAREN,")");
            consume(Token::Type::L_BRACE,"{");
            auto body = block();
            return make_shared<LambdaExpr>(params, body);
        }
        if(match({Token::Type::L_PAREN})){ auto e = expression(); consume(Token::Type::R_PAREN,")"); return e; }
        return make_shared<LiteralExpr>(Value::make_nil());
    }
};



struct Interpreter {
    shared_ptr<Environment> globals;
    mt19937 rng;
    Interpreter(){ globals = make_shared<Environment>(nullptr); rng.seed((unsigned)time(nullptr)); defineBuiltins(); }

    static double toDouble(const shared_ptr<Value>& v){ if(!v) return 0.0; if(v->kind==ValueKind::NUMBER) return v->number_value; if(v->kind==ValueKind::INT) return (double)v->int_value; if(v->kind==ValueKind::COMPLEX) return v->complex_value.real(); return 0.0; }
    static bool isTruthy(const shared_ptr<Value>& v){ if(!v) return false; if(v->kind==ValueKind::NIL) return false; if(v->kind==ValueKind::BOOL) return v->bool_value; if(v->kind==ValueKind::NUMBER) return v->number_value!=0.0; if(v->kind==ValueKind::INT) return v->int_value!=0; return true; }

    
    void defineBuiltins() {
        auto unary_elementwise = [this](function<double(double)> f)
            -> function<shared_ptr<Value>(const vector<shared_ptr<Value>>&)>
            {
                return [this, f](const vector<shared_ptr<Value>>& args) -> shared_ptr<Value> {
                    if (args.empty()) return Value::make_number(0.0);
                    auto a = args[0];
                    // 数组广播
                    if (a->kind == ValueKind::ARRAY) {
                        auto out = Value::make_array();
                        for (auto& e : a->array_value) {
                            double v = (e->kind == ValueKind::INT) ? e->int_value :
                                (e->kind == ValueKind::NUMBER) ? e->number_value : 0.0;
                            out->array_value.push_back(Value::make_number(f(v)));
                        }
                        return out;
                    }
                    // NDArray 广播
                    if (a->kind == ValueKind::NDARRAY) {
                        auto out = make_shared<NDArray>(a->ndarray_value->dims);
                        for (size_t i = 0; i < a->ndarray_value->data.size(); ++i) {
                            out->data[i] = complex<double>(f(a->ndarray_value->data[i].real()), 0.0);
                        }
                        return Value::make_ndarray(out);
                    }
                    // 标量
                    double v = (a->kind == ValueKind::INT) ? a->int_value :
                        (a->kind == ValueKind::NUMBER) ? a->number_value : 0.0;
                    return Value::make_number(f(v));
                    };
            };
        globals->define("__builtin_print", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            for (size_t i = 0; i < args.size(); ++i) { if (i) cout << " "; cout << args[i]->toString(); } cout << "\n";
            return Value::make_nil();
            }));
        globals->define("clear", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_bool(false);
            auto a = args[0];
            if (a->kind == ValueKind::ARRAY) { a->array_value.clear(); return Value::make_bool(true); }
            if (a->kind == ValueKind::MAP) { a->map_value.clear(); return Value::make_bool(true); }
            if (a->kind == ValueKind::NDARRAY) { a->ndarray_value->data.assign(a->ndarray_value->data.size(), complex<double>(0.0, 0.0)); return Value::make_bool(true); }
            return Value::make_bool(false);
            }));
        globals->define("make_complex", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            double re = 0, im = 0;
            if (args.size() > 0) { re = toDouble(args[0]); if (args.size() > 1) im = toDouble(args[1]); }
            return Value::make_complex({ re, im });
            }));
        globals->define("matmul", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_nil();
            auto A = args[0]; auto B = args[1];
            if (A->kind != ValueKind::NDARRAY || B->kind != ValueKind::NDARRAY) return Value::make_nil();
            if (A->ndarray_value->dims.size() != 2 || B->ndarray_value->dims.size() != 2) return Value::make_nil();
            size_t r = A->ndarray_value->dims[0], k = A->ndarray_value->dims[1], c = B->ndarray_value->dims[1];
            if (k != B->ndarray_value->dims[0]) return Value::make_nil();
            auto out = make_shared<NDArray>(vector<size_t>{r, c});
#pragma omp parallel for if(r*c>=1024)
            for (size_t col = 0; col < c; ++col) {
                for (size_t row = 0; row < r; ++row) {
                    complex<double> s = 0.0;
                    for (size_t kk = 0; kk < k; ++kk) {
                        s += A->ndarray_value->data[kk * r + row] * B->ndarray_value->data[col * B->ndarray_value->dims[0] + kk];
                    }
                    out->data[col * r + row] = s;
                }
            }
            return Value::make_ndarray(out);
            }));
        globals->define("transpose", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_nil();
            auto A = args[0];
            if (A->kind != ValueKind::NDARRAY) return Value::make_nil();
            if (A->ndarray_value->dims.size() != 2) return Value::make_ndarray(A->ndarray_value);
            auto out = make_shared<NDArray>(vector<size_t>{A->ndarray_value->dims[1], A->ndarray_value->dims[0]});
            size_t r = A->ndarray_value->dims[0], c = A->ndarray_value->dims[1];
            for (size_t i = 0; i < r; ++i) for (size_t j = 0; j < c; ++j) out->data[i + j * r] = A->ndarray_value->data[j + i * c];
            return Value::make_ndarray(out);
            }));

        globals->define("log", Value::make_native(unary_elementwise([](double x) { return log(x); })));
        globals->define("exp", Value::make_native(unary_elementwise([](double x) { return exp(x); })));
        globals->define("sqrt", Value::make_native(unary_elementwise([](double x) { return sqrt(x); })));
        globals->define("cbrt", Value::make_native(unary_elementwise([](double x) { return cbrt(x); })));
        globals->define("sin", Value::make_native(unary_elementwise([](double x) { return sin(x); })));
        globals->define("cos", Value::make_native(unary_elementwise([](double x) { return cos(x); })));
        globals->define("tan", Value::make_native(unary_elementwise([](double x) { return tan(x); })));
        globals->define("asin", Value::make_native(unary_elementwise([](double x) { return asin(x); })));
        globals->define("acos", Value::make_native(unary_elementwise([](double x) { return acos(x); })));
        globals->define("atan", Value::make_native(unary_elementwise([](double x) { return atan(x); })));
        globals->define("atan2", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_number(0.0);
            return Value::make_number(atan2(toDouble(args[0]), toDouble(args[1])));
            }));
        globals->define("sinh", Value::make_native(unary_elementwise([](double x) { return sinh(x); })));
        globals->define("cosh", Value::make_native(unary_elementwise([](double x) { return cosh(x); })));
        globals->define("tanh", Value::make_native(unary_elementwise([](double x) { return tanh(x); })));
        globals->define("tgamma", Value::make_native(unary_elementwise([](double x) { return tgamma(x); })));
        globals->define("erf", Value::make_native(unary_elementwise([](double x) { return erf(x); })));
        globals->define("pow", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_number(0.0);
            return Value::make_number(pow(toDouble(args[0]), toDouble(args[1])));
            }));

        globals->define("random", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            uniform_real_distribution<double> dist(0.0, 1.0);
            return Value::make_number(dist(rng));
            }));
        globals->define("noise", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_number(0.0);
            double x = toDouble(args[0]);
            double v = sin(x * 12.9898 + 78.233) * 43758.5453;
            return Value::make_number(v - floor(v));
            }));
        globals->define("floor", Value::make_native(unary_elementwise([](double x) { return floor(x); })));
        globals->define("ceil", Value::make_native(unary_elementwise([](double x) { return ceil(x); })));
        globals->define("round", Value::make_native(unary_elementwise([](double x) { return round(x); })));
        globals->define("trunc", Value::make_native(unary_elementwise([](double x) { return trunc(x); })));
        globals->define("fract", Value::make_native(unary_elementwise([](double x) { return x - floor(x); })));
        globals->define("abs", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_number(0.0);
            auto a = args[0];
            if (a->kind == ValueKind::INT) return Value::make_int(llabs(a->int_value));
            if (a->kind == ValueKind::NUMBER) return Value::make_number(fabs(a->number_value));
            return Value::make_number(0.0);
            }));
        globals->define("sign", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_int(0);
            double d = toDouble(args[0]);
            if (d > 0) return Value::make_int(1);
            if (d < 0) return Value::make_int(-1);
            return Value::make_int(0);
            }));
        globals->define("min", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_number(0.0);
            if (args.size() == 1 && args[0]->kind == ValueKind::ARRAY) {
                double m = numeric_limits<double>::infinity();
                for (auto& e : args[0]->array_value) m = std::min(m, toDouble(e));
                return Value::make_number(m);
            }
            double m = toDouble(args[0]);
            for (size_t i = 1; i < args.size(); ++i) m = std::min(m, toDouble(args[i]));
            return Value::make_number(m);
            }));
        globals->define("max", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty()) return Value::make_number(0.0);
            if (args.size() == 1 && args[0]->kind == ValueKind::ARRAY) {
                double M = -numeric_limits<double>::infinity();
                for (auto& e : args[0]->array_value) M = std::max(M, toDouble(e));
                return Value::make_number(M);
            }
            double M = toDouble(args[0]);
            for (size_t i = 1; i < args.size(); ++i) M = std::max(M, toDouble(args[i]));
            return Value::make_number(M);
            }));
        globals->define("clamp", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 3) return Value::make_number(0.0);
            double x = toDouble(args[0]), a = toDouble(args[1]), b = toDouble(args[2]);
            return Value::make_number(std::max(a, std::min(b, x)));
            }));
        globals->define("median", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty() || args[0]->kind != ValueKind::ARRAY) return Value::make_number(0.0);
            vector<double> arr;
            for (auto& e : args[0]->array_value) arr.push_back(toDouble(e));
            if (arr.empty()) return Value::make_number(0.0);
            sort(arr.begin(), arr.end());
            if (arr.size() % 2 == 1) return Value::make_number(arr[arr.size() / 2]);
            return Value::make_number((arr[arr.size() / 2 - 1] + arr[arr.size() / 2]) / 2.0);
            }));
        globals->define("sort", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty() || args[0]->kind != ValueKind::ARRAY) return Value::make_array();
            auto out = Value::make_array();
            for (auto& e : args[0]->array_value) out->array_value.push_back(e);
            sort(out->array_value.begin(), out->array_value.end(), [this](const shared_ptr<Value>& a, const shared_ptr<Value>& b) {
                return toDouble(a) < toDouble(b);
                });
            return out;
            }));
        globals->define("sigmoid", Value::make_native(unary_elementwise([](double x) { return 1.0 / (1.0 + exp(-x)); })));
        globals->define("step", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_number(0.0);
            double edge = toDouble(args[0]), x = toDouble(args[1]);
            return Value::make_number(x < edge ? 0.0 : 1.0);
            }));
        globals->define("smoothstep", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 3) return Value::make_number(0.0);
            double e0 = toDouble(args[0]), e1 = toDouble(args[1]), x = toDouble(args[2]);
            double t = (x - e0) / (e1 - e0);
            if (t < 0) t = 0; else if (t > 1) t = 1;
            return Value::make_number(t * t * (3 - 2 * t));
            }));
        globals->define("saturate", Value::make_native(unary_elementwise([](double x) {
            if (x < 0) return 0.0; if (x > 1) return 1.0; return x;
            })));

#if defined(__unix__) || defined(__APPLE__)
        globals->define("tcp_connect", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_int(-1);
            string host = args[0]->kind == ValueKind::STRING ? args[0]->string_value : "";
            int port = (int)toDouble(args[1]);
            struct addrinfo hints {}, * res;
            hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
            string sport = to_string(port);
            if (getaddrinfo(host.c_str(), sport.c_str(), &hints, &res) != 0) return Value::make_int(-1);
            int sock = -1;
            for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
                sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (sock < 0) continue;
                if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
                close(sock); sock = -1;
            }
            freeaddrinfo(res);
            return Value::make_int(sock);
            }));
        globals->define("socket_send", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_int(-1);
            int fd = (args[0]->kind == ValueKind::INT) ? (int)args[0]->int_value : -1;
            string data = (args[1]->kind == ValueKind::STRING) ? args[1]->string_value : "";
            if (fd < 0) return Value::make_int(-1);
            ssize_t sent = send(fd, data.c_str(), data.size(), 0);
            return Value::make_int((int64_t)sent);
            }));
        globals->define("socket_recv", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.size() < 2) return Value::make_string("");
            int fd = (args[0]->kind == ValueKind::INT) ? (int)args[0]->int_value : -1;
            int n = (int)toDouble(args[1]);
            if (fd < 0) return Value::make_string("");
            string buf; buf.resize(n);
            ssize_t rec = recv(fd, &buf[0], n, 0);
            if (rec <= 0) return Value::make_string("");
            buf.resize((size_t)rec);
            return Value::make_string(buf);
            }));
        globals->define("http_get", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value> {
            if (args.empty() || args[0]->kind != ValueKind::STRING) return Value::make_string("");
            string url = args[0]->string_value;
            if (url.rfind("http://", 0) != 0) return Value::make_string("");
            string rest = url.substr(7);
            string host; int port = 80; string path = "/";
            size_t p = rest.find('/');
            if (p == string::npos) host = rest;
            else { host = rest.substr(0, p); path = rest.substr(p); }
            size_t cp = host.find(':');
            if (cp != string::npos) { port = stoi(host.substr(cp + 1)); host = host.substr(0, cp); }

            auto fdv = globals->get("tcp_connect")->native_fn({ Value::make_string(host), Value::make_int(port) });
            if (fdv->kind != ValueKind::INT || fdv->int_value < 0) return Value::make_string("");
            int fd = (int)fdv->int_value;
            string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
            globals->get("socket_send")->native_fn({ Value::make_int(fd), Value::make_string(req) });
            string out;
            char buf[4096];
            while (true) {
                ssize_t r = recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) break;
                out.append(buf, buf + r);
            }
            close(fd);
            return Value::make_string(out);
            }));
#else
        globals->define("tcp_connect", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_int(-1); }));
        globals->define("socket_send", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_int(-1); }));
        globals->define("socket_recv", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_string(""); }));
        globals->define("http_get", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_string(""); }));
#endif

#ifdef HAS_MPI
        globals->define("mpi_init", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { MPI_Init(nullptr, nullptr); return Value::make_nil(); }));
        globals->define("mpi_finalize", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { MPI_Finalize(); return Value::make_nil(); }));
        globals->define("mpi_rank", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { int r; MPI_Comm_rank(MPI_COMM_WORLD, &r); return Value::make_int(r); }));
        globals->define("mpi_size", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { int s; MPI_Comm_size(MPI_COMM_WORLD, &s); return Value::make_int(s); }));
#else
        globals->define("mpi_init", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_nil(); }));
        globals->define("mpi_finalize", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_nil(); }));
        globals->define("mpi_rank", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_int(0); }));
        globals->define("mpi_size", Value::make_native([](const vector<shared_ptr<Value>>&)->shared_ptr<Value> { return Value::make_int(1); }));
#endif
        // numeric integrate: integrate(fn, a, b, n)
        globals->define("integrate", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value>{
            if(args.size()<4) return Value::make_number(0.0);
            auto fn = args[0]; double a = toDouble(args[1]); double b = toDouble(args[2]); int n = (int) (args[3]->kind==ValueKind::INT?args[3]->int_value:(int)args[3]->number_value);
            if(n<=0) n=1000;
            double h = (b-a)/n; double s=0;
            for(int i=0;i<=n;i++){
                double x = a + i*h;
                double fx = 0.0;
                if(fn->kind==ValueKind::NATIVE){
                    auto r = fn->native_fn(vector<shared_ptr<Value>>{Value::make_number(x)});
                    fx = toDouble(r);
                } else if(fn->kind==ValueKind::FUNCTION){
                    VM vm(this->globals);
                    auto r = vm.run(fn->fn_value, vector<shared_ptr<Value>>{Value::make_number(x)});
                    fx = toDouble(r);
                }
                if(i==0 || i==n) s += fx;
                else if(i%2==0) s += 2*fx;
                else s += 4*fx;
            }
            double res = s * h / 3.0;
            return Value::make_number(res);
        }));
        // ODE solver RK4: ode_solve(f, y0, t0, t1, dt) f(t,y) returns dy/dt (scalar)
        globals->define("ode_solve", Value::make_native([this](const vector<shared_ptr<Value>>& args)->shared_ptr<Value>{
            if(args.size()<5) return Value::make_array();
            auto f = args[0];
            double y = toDouble(args[1]); double t0 = toDouble(args[2]); double t1=toDouble(args[3]); double dt = toDouble(args[4]);
            vector<double> sol_t; vector<double> sol_y;
            double t = t0; double cur = y;
            sol_t.push_back(t); sol_y.push_back(cur);
            while(t < t1 - 1e-12){
                double h = min(dt, t1 - t);
                auto eval_f = [&](double tt, double yy)->double{
                    if(f->kind==ValueKind::NATIVE){
                        auto r = f->native_fn(vector<shared_ptr<Value>>{Value::make_number(tt), Value::make_number(yy)});
                        return toDouble(r);
                    } else if(f->kind==ValueKind::FUNCTION){
                        VM vm(this->globals);
                        auto r = vm.run(f->fn_value, vector<shared_ptr<Value>>{Value::make_number(tt), Value::make_number(yy)});
                        return toDouble(r);
                    }
                    return 0.0;
                };
                double k1 = eval_f(t, cur);
                double k2 = eval_f(t + h/2.0, cur + h*k1/2.0);
                double k3 = eval_f(t + h/2.0, cur + h*k2/2.0);
                double k4 = eval_f(t + h, cur + h*k3);
                cur = cur + h*(k1 + 2*k2 + 2*k3 + k4)/6.0;
                t += h;
                sol_t.push_back(t); sol_y.push_back(cur);
            }
            auto out = Value::make_array();
            for(size_t i=0;i<sol_t.size();++i){
                auto pairv = Value::make_array();
                pairv->array_value.push_back(Value::make_number(sol_t[i]));
                pairv->array_value.push_back(Value::make_number(sol_y[i]));
                out->array_value.push_back(pairv);
            }
            return out;
        }));
    }

    // assign to target
    shared_ptr<Value> assignToTarget(shared_ptr<Expr> target, shared_ptr<Value> rhs, shared_ptr<Environment> env){
        if(!target) return Value::make_nil();
        if(auto v = dynamic_pointer_cast<VariableExpr>(target)){
            if(env->existsLocal(v->name)) env->assign(v->name, rhs);
            else env->define(v->name, rhs, true);
            return rhs;
        }
        if(auto idx = dynamic_pointer_cast<IndexExpr>(target)){
            auto obj = evalExpr(idx->object, env);
            auto ind = evalExpr(idx->index, env);
            if(obj->kind==ValueKind::ARRAY && (ind->kind==ValueKind::INT || ind->kind==ValueKind::NUMBER)){
                int i = (ind->kind==ValueKind::INT)? (int)ind->int_value : (int)ind->number_value;
                if(i>=0 && i < (int)obj->array_value.size()) obj->array_value[i] = rhs;
            } else if(obj->kind==ValueKind::NDARRAY && ind->kind==ValueKind::INT){
                // flat index assignment for NDARRAY (simple)
                int idx0 = (int)ind->int_value;
                if(idx0 >=0 && idx0 < (int)obj->ndarray_value->data.size()){
                    if(rhs->kind==ValueKind::COMPLEX) obj->ndarray_value->data[idx0] = rhs->complex_value;
                    else obj->ndarray_value->data[idx0] = complex<double>(toDouble(rhs), 0.0);
                }
            }
            return rhs;
        }
        if(auto mem = dynamic_pointer_cast<MemberExpr>(target)){
            auto obj = evalExpr(mem->object, env);
            if(obj->kind==ValueKind::MAP) obj->map_value[mem->name] = rhs;
            return rhs;
        }
        // destructuring left as earlier; omitted for brevity
        return rhs;
    }

    // Evaluate expressions (subset with requested features)
    shared_ptr<Value> evalExpr(shared_ptr<Expr> e, shared_ptr<Environment> env){
        if(!e) return Value::make_nil();
        if(auto lit = dynamic_pointer_cast<LiteralExpr>(e)) return lit->val;
        if(auto var = dynamic_pointer_cast<VariableExpr>(e)){
            auto v = env->get(var->name);
            return v ? v : Value::make_nil();
        }
        if(auto asg = dynamic_pointer_cast<AssignExpr>(e)){
            auto val = evalExpr(asg->value, env);
            return assignToTarget(asg->target, val, env);
        }
        if(auto bin = dynamic_pointer_cast<BinaryExpr>(e)){
            auto L = evalExpr(bin->left, env);
            auto R = evalExpr(bin->right, env);
            // vectorized NDARRAY addition
            if(bin->op == "+"){
                if(L->kind == ValueKind::NDARRAY && R->kind == ValueKind::NDARRAY){
                    // broadcasting only when shapes equal
                    if(L->ndarray_value->dims == R->ndarray_value->dims){
                        auto out = make_shared<NDArray>(L->ndarray_value->dims);
                        size_t n = L->ndarray_value->data.size();
                        for(size_t i=0;i<n;++i) out->data[i] = L->ndarray_value->data[i] + R->ndarray_value->data[i];
                        return Value::make_ndarray(out);
                    }
                }
                if(L->kind == ValueKind::COMPLEX || R->kind==ValueKind::COMPLEX){
                    complex<double> a = (L->kind==ValueKind::COMPLEX) ? L->complex_value : complex<double>(toDouble(L),0.0);
                    complex<double> b = (R->kind==ValueKind::COMPLEX) ? R->complex_value : complex<double>(toDouble(R),0.0);
                    return Value::make_complex(a+b);
                }
                if(L->kind==ValueKind::INT && R->kind==ValueKind::INT) return Value::make_int(L->int_value + R->int_value);
                return Value::make_number(toDouble(L) + toDouble(R));
            }
            if(bin->op == "-"){ if(L->kind==ValueKind::INT && R->kind==ValueKind::INT) return Value::make_int(L->int_value - R->int_value); return Value::make_number(toDouble(L)-toDouble(R)); }
            if(bin->op == "*"){
                // treat NDARRAY * NDARRAY as elementwise, matmul is builtin
                if(L->kind==ValueKind::NDARRAY && R->kind==ValueKind::NDARRAY){
                    if(L->ndarray_value->dims == R->ndarray_value->dims){
                        auto out = make_shared<NDArray>(L->ndarray_value->dims);
                        size_t n = out->data.size();
                        for(size_t i=0;i<n;++i) out->data[i] = L->ndarray_value->data[i] * R->ndarray_value->data[i];
                        return Value::make_ndarray(out);
                    }
                }
                if(L->kind==ValueKind::COMPLEX || R->kind==ValueKind::COMPLEX){
                    complex<double> a = (L->kind==ValueKind::COMPLEX)?L->complex_value:complex<double>(toDouble(L),0.0);
                    complex<double> b = (R->kind==ValueKind::COMPLEX)?R->complex_value:complex<double>(toDouble(R),0.0);
                    return Value::make_complex(a*b);
                }
                if(L->kind==ValueKind::INT && R->kind==ValueKind::INT) return Value::make_int(L->int_value * R->int_value);
                return Value::make_number(toDouble(L) * toDouble(R));
            }
            if(bin->op == "/") return Value::make_number(toDouble(L) / toDouble(R));
            if(bin->op == "==") return Value::make_bool(L->toString() == R->toString());
            if(bin->op == "and") return Value::make_bool(isTruthy(L) && isTruthy(R));
            if(bin->op == "or") return Value::make_bool(isTruthy(L) || isTruthy(R));
            return Value::make_nil();
        }
        if(auto un = dynamic_pointer_cast<UnaryExpr>(e)){
            auto R = evalExpr(un->right, env);
            if(un->op == "-"){ if(R->kind==ValueKind::INT) return Value::make_int(-R->int_value); if(R->kind==ValueKind::COMPLEX) return Value::make_complex(-R->complex_value); return Value::make_number(-toDouble(R)); }
            if(un->op == "!") return Value::make_bool(!isTruthy(R));
            return Value::make_nil();
        }
        if(auto call = dynamic_pointer_cast<CallExpr>(e)){
            auto cal = evalExpr(call->callee, env);
            vector<shared_ptr<Value>> args;
            for(auto &a: call->args){
                if(auto sp = dynamic_pointer_cast<SpreadExpr>(a)){
                    auto arr = evalExpr(sp->inner, env);
                    if(arr->kind==ValueKind::ARRAY){
                        for(auto &v: arr->array_value) args.push_back(v);
                    }
                } else args.push_back(evalExpr(a, env));
            }
            if(cal->kind==ValueKind::NATIVE) return cal->native_fn(args);
            if(cal->kind==ValueKind::FUNCTION && cal->fn_value){
                VM vm(this->globals);
                return vm.run(cal->fn_value, args);
            }
            return Value::make_nil();
        }
        if(auto arr = dynamic_pointer_cast<ArrayExpr>(e)){
            // try to create NDArray if nested numeric with uniform dimensions
            bool nested = !arr->elements.empty();
            bool allNum = true;
            for(auto &el: arr->elements){
                auto v = dynamic_pointer_cast<LiteralExpr>(el);
                if(!v) { allNum = false; break; }
                if(!(v->val->kind==ValueKind::NUMBER || v->val->kind==ValueKind::INT)) { allNum = false; break; }
            }
            // If not uniform numeric or nested arrays, create generic array
            auto out = Value::make_array();
            for(auto &el: arr->elements) out->array_value.push_back(evalExpr(el, env));
            return out;
        }
        if(auto mp = dynamic_pointer_cast<MapExpr>(e)){
            auto out = Value::make_map();
            for(auto &p: mp->pairs) out->map_value[p.first] = evalExpr(p.second, env);
            return out;
        }
        if(auto idx = dynamic_pointer_cast<IndexExpr>(e)){
            auto obj = evalExpr(idx->object, env);
            auto ind = evalExpr(idx->index, env);
            if(obj->kind==ValueKind::ARRAY && (ind->kind==ValueKind::INT||ind->kind==ValueKind::NUMBER)){
                int i = (ind->kind==ValueKind::INT)? (int)ind->int_value : (int)ind->number_value;
                if(i<0 || i>=(int)obj->array_value.size()) return Value::make_nil();
                return obj->array_value[i];
            }
            if(obj->kind==ValueKind::NDARRAY && ind->kind==ValueKind::INT){
                int i = (int)ind->int_value;
                if(i>=0 && i < (int)obj->ndarray_value->data.size()){
                    return Value::make_complex(obj->ndarray_value->data[i]);
                }
            }
            return Value::make_nil();
        }
        if(auto sl = dynamic_pointer_cast<SliceExpr>(e)){
            auto obj = evalExpr(sl->object, env);
            if(obj->kind==ValueKind::ARRAY){
                // simple 1D slicing: [start:stop:step]
                int n = (int)obj->array_value.size();
                int start = sl->start ? (int) (evalExpr(sl->start, env)->kind==ValueKind::INT? evalExpr(sl->start, env)->int_value : (int)evalExpr(sl->start, env)->number_value) : 0;
                int stop  = sl->stop  ? (int) (evalExpr(sl->stop, env)->kind==ValueKind::INT? evalExpr(sl->stop, env)->int_value : (int)evalExpr(sl->stop, env)->number_value) : n;
                int step  = sl->step  ? (int) (evalExpr(sl->step, env)->kind==ValueKind::INT? evalExpr(sl->step, env)->int_value : (int)evalExpr(sl->step, env)->number_value) : 1;
                if(step==0) step=1;
                auto out = Value::make_array();
                if(start<0) start += n; if(stop<0) stop += n;
                for(int i=start;i<stop;i+=step){ if(i>=0 && i<n) out->array_value.push_back(obj->array_value[i]); }
                return out;
            }
            if(obj->kind==ValueKind::NDARRAY){
                // slicing NDArray returns a view / copy of flattened slice (simple)
                int size = (int)obj->ndarray_value->data.size();
                int start = sl->start ? (int) (evalExpr(sl->start, env)->kind==ValueKind::INT? evalExpr(sl->start, env)->int_value : (int)evalExpr(sl->start, env)->number_value) : 0;
                int stop  = sl->stop  ? (int) (evalExpr(sl->stop, env)->kind==ValueKind::INT? evalExpr(sl->stop, env)->int_value : (int)evalExpr(sl->stop, env)->number_value) : size;
                int step  = sl->step  ? (int) (evalExpr(sl->step, env)->kind==ValueKind::INT? evalExpr(sl->step, env)->int_value : (int)evalExpr(sl->step, env)->number_value) : 1;
                if(step==0) step=1;
                if(start<0) start += size; if(stop<0) stop += size;
                vector<size_t> dims = { (size_t) max(0, (stop-start+step-1)/step) };
                auto out_nd = make_shared<NDArray>(dims);
                size_t p=0;
                for(int i=start;i<stop;i+=step){ if(i>=0 && i<size) out_nd->data[p++] = obj->ndarray_value->data[i]; }
                return Value::make_ndarray(out_nd);
            }
            return Value::make_nil();
        }
        if(auto mem = dynamic_pointer_cast<MemberExpr>(e)){
            auto obj = evalExpr(mem->object, env);
            if(obj->kind==ValueKind::MAP){
                auto it = obj->map_value.find(mem->name);
                if(it!=obj->map_value.end()) return it->second;
            }
            // common methods
            if(mem->name=="len"){
                if(obj->kind==ValueKind::ARRAY) return Value::make_int((int64_t)obj->array_value.size());
                if(obj->kind==ValueKind::STRING) return Value::make_int((int64_t)obj->string_value.size());
                if(obj->kind==ValueKind::NDARRAY) return Value::make_int((int64_t)obj->ndarray_value->size());
                return Value::make_int(0);
            }
            return Value::make_nil();
        }
        if(auto lam = dynamic_pointer_cast<LambdaExpr>(e)){
            Compiler c;
            for(size_t i=0;i<lam->params.size();++i) c.localIndex[lam->params[i]] = (int)i;
            c.fn->params = lam->params;
            for(auto &st: lam->body) c.compileStmt(st);
            c.emit(FunctionObj::OP_CONST, c.addConst(Value::make_nil()));
            c.emit(FunctionObj::OP_RETURN);
            return Value::make_function(c.fn);
        }
        if(auto sp = dynamic_pointer_cast<SpreadExpr>(e)) return evalExpr(sp->inner, env);
        if(auto me = dynamic_pointer_cast<MatchExpr>(e)){
            auto dv = evalExpr(me->discr, env);
            for(auto &arm : me->arms){
                // pattern matching: literal or var or array/map destructuring (basic)
                if(auto litpat = dynamic_pointer_cast<LiteralExpr>(arm.pattern)){
                    auto pv = evalExpr(arm.pattern, env);
                    if(pv->toString() == dv->toString()){
                        auto local = make_shared<Environment>(env);
                        return evalExpr(arm.body, local);
                    }
                } else if(auto vpat = dynamic_pointer_cast<VariableExpr>(arm.pattern)){
                    if(vpat->name == "_"){
                        auto local = make_shared<Environment>(env);
                        return evalExpr(arm.body, local);
                    } else {
                        auto local = make_shared<Environment>(env);
                        local->define(vpat->name, dv, true);
                        return evalExpr(arm.body, local);
                    }
                } else if(auto apat = dynamic_pointer_cast<ArrayExpr>(arm.pattern)){
                    // array destructuring pattern: match only for arrays simple equality of lengths and assign elements
                    if(dv->kind==ValueKind::ARRAY && apat->elements.size() == dv->array_value.size()){
                        auto local = make_shared<Environment>(env);
                        for(size_t i=0;i<apat->elements.size();++i){
                            if(auto v = dynamic_pointer_cast<VariableExpr>(apat->elements[i])){
                                local->define(v->name, dv->array_value[i], true);
                            }
                        }
                        return evalExpr(arm.body, local);
                    }
                } else if(auto mpat = dynamic_pointer_cast<MapExpr>(arm.pattern)){
                    if(dv->kind==ValueKind::MAP){
                        auto local = make_shared<Environment>(env);
                        bool ok=true;
                        for(auto &p: mpat->pairs){
                            auto it = dv->map_value.find(p.first);
                            if(it==dv->map_value.end()){ ok=false; break; }
                            if(auto v = dynamic_pointer_cast<VariableExpr>(p.second)){
                                local->define(v->name, it->second, true);
                            }
                        }
                        if(ok) return evalExpr(arm.body, local);
                    }
                }
            }
            if(me->defaultArm) return evalExpr(me->defaultArm, env);
            return Value::make_nil();
        }
        return Value::make_nil();
    }

    // Execute statements with support for labels/goto within a block
    shared_ptr<Value> executeStmt(shared_ptr<Stmt> s, shared_ptr<Environment> env){
        if(!s) return Value::make_nil();
        if (auto ps = dynamic_pointer_cast<PrintStmt>(s)) {
            vector<shared_ptr<Value>> vals;
            for (auto& a : ps->args) vals.push_back(evalExpr(a, env));
            auto print_fn = globals->get("__builtin_print");
            if (print_fn && print_fn->kind == ValueKind::NATIVE) {
                print_fn->native_fn(vals);
            }
            return Value::make_nil();
        }
        // Expression statement
        if(auto es = dynamic_pointer_cast<ExprStmt>(s)) return evalExpr(es->expr, env);
        if(auto vs = dynamic_pointer_cast<VarStmt>(s)){
            auto val = vs->initializer ? evalExpr(vs->initializer, env) : Value::make_nil();
            assignToPattern(vs->pattern, val, env);
            if(auto vn = dynamic_pointer_cast<VariableExpr>(vs->pattern)){
                auto e = env->findEntry(vn->name);
                if(e) e->is_mut = vs->is_mut;
            }
            return val;
        }
        if(auto lbl = dynamic_pointer_cast<LabelStmt>(s)){
            // no-op at runtime; labels are processed in block executor
            return Value::make_nil();
        }
        if(auto gs = dynamic_pointer_cast<GotoStmt>(s)){
            throw GotoException(gs->name);
        }
        if(auto ps = dynamic_pointer_cast<PushStmt>(s)){
            // only support variable target
            if(auto v = dynamic_pointer_cast<VariableExpr>(ps->target)){
                auto arr = env->get(v->name);
                if(!arr || arr->kind!=ValueKind::ARRAY){
                    arr = Value::make_array();
                    env->define(v->name, arr, true);
                }
                for(auto &valE : ps->vals){
                    if(auto sp = dynamic_pointer_cast<SpreadExpr>(valE)){
                        auto a = evalExpr(sp->inner, env);
                        if(a->kind==ValueKind::ARRAY){
                            for(auto &x: a->array_value) arr->array_value.push_back(x);
                        }
                    } else {
                        arr->array_value.push_back(evalExpr(valE, env));
                    }
                }
            } else {
                // evaluate target and append locally (no effect outside)
                auto tgt = evalExpr(ps->target, env);
                if(tgt->kind==ValueKind::ARRAY){
                    for(auto &valE: ps->vals){
                        if(auto sp = dynamic_pointer_cast<SpreadExpr>(valE)){
                            auto a = evalExpr(sp->inner, env);
                            if(a->kind==ValueKind::ARRAY){
                                for(auto &x: a->array_value) tgt->array_value.push_back(x);
                            }
                        } else tgt->array_value.push_back(evalExpr(valE, env));
                    }
                }
            }
            return Value::make_nil();
        }
        if(auto fs = dynamic_pointer_cast<FunctionStmt>(s)){
            Compiler c;
            for(size_t i=0;i<fs->params.size();++i) c.localIndex[fs->params[i]] = (int)i;
            c.fn->params = fs->params;
            for(auto &st: fs->body) c.compileStmt(st);
            c.emit(FunctionObj::OP_CONST, c.addConst(Value::make_nil()));
            c.emit(FunctionObj::OP_RETURN);
            env->define(fs->name, Value::make_function(c.fn), true);
            return Value::make_nil();
        }
        if(auto ts = dynamic_pointer_cast<TryCatchStmt>(s)){
            try {
                return executeStmt(ts->tryBlock, env);
            } catch(ThrowException &ex){
                if(!ts->exName.empty() && ts->catchBlock){
                    auto local = make_shared<Environment>(env);
                    local->define(ts->exName, ex.v, true);
                    return executeStmt(ts->catchBlock, local);
                }
                throw;
            }
        }
        if(auto thr = dynamic_pointer_cast<ThrowStmt>(s)){
            auto v = evalExpr(thr->expr, env);
            throw ThrowException(v);
        }
        if(auto bs = dynamic_pointer_cast<BlockStmt>(s)){
            // build label map
            unordered_map<string, size_t> label_map;
            for(size_t i=0;i<bs->statements.size();++i){
                if(auto lbl = dynamic_pointer_cast<LabelStmt>(bs->statements[i])){
                    label_map[lbl->name] = i;
                }
            }
            // execute statements by index to support goto
            shared_ptr<Value> last = Value::make_nil();
            for(size_t ip=0; ip<bs->statements.size(); ){
                try {
                    auto st = bs->statements[ip];
                    // skip label nodes
                    if(dynamic_pointer_cast<LabelStmt>(st)){ ++ip; continue; }
                    last = executeStmt(st, env);
                    ++ip;
                } catch(GotoException &g){
                    auto it = label_map.find(g.label);
                    if(it==label_map.end()) throw runtime_error("label not found: " + g.label);
                    ip = it->second + 1; // move to statement after label
                    continue;
                } catch(BreakException &b){ throw; } catch(ContinueException &c) { throw; } catch(ThrowException &t){ throw; }
            }
            return last;
        }
        if(auto ifs = dynamic_pointer_cast<IfStmt>(s)){
            auto c = evalExpr(ifs->cond, env);
            if(isTruthy(c)) return executeStmt(ifs->thenB, env);
            else if(ifs->elseB) return executeStmt(ifs->elseB, env);
            return Value::make_nil();
        }
        if(auto ws = dynamic_pointer_cast<WhileStmt>(s)){
            while(isTruthy(evalExpr(ws->cond, env))){
                try {
                    executeStmt(ws->body, env);
                } catch(BreakException &){ break; } catch(ContinueException &){ continue; }
            }
            return Value::make_nil();
        }
        if(auto fs = dynamic_pointer_cast<ForInStmt>(s)){
            auto itv = evalExpr(fs->iterable, env);
            if(itv->kind==ValueKind::ARRAY){
                for(auto &elem : itv->array_value){
                    auto prev = env;
                    env = make_shared<Environment>(prev);
                    env->define(fs->var, elem, true);
                    try{ executeStmt(fs->body, env); } catch(BreakException &){ env = prev; break; } catch(ContinueException &){ env = prev; continue; }
                    env = prev;
                }
            }
            return Value::make_nil();
        }
        if(auto rs = dynamic_pointer_cast<ReturnStmt>(s)){
            return rs->value ? evalExpr(rs->value, env) : Value::make_nil();
        }
        return Value::make_nil();
    }

    // pattern assignment support
    void assignToPattern(shared_ptr<Expr> pattern, shared_ptr<Value> rhs, shared_ptr<Environment> env){
        if(!pattern) return;
        if(auto v = dynamic_pointer_cast<VariableExpr>(pattern)){
            if(env->existsLocal(v->name)) env->assign(v->name, rhs);
            else env->define(v->name, rhs, true);
            return;
        }
        if(auto a = dynamic_pointer_cast<ArrayExpr>(pattern)){
            if(rhs->kind != ValueKind::ARRAY) return;
            size_t vi=0;
            for(size_t i=0;i<a->elements.size();++i){
                auto el = a->elements[i];
                if(auto sp = dynamic_pointer_cast<SpreadExpr>(el)){
                    // rest
                    auto rest = Value::make_array();
                    for(size_t k=vi;k<rhs->array_value.size();++k) rest->array_value.push_back(rhs->array_value[k]);
                    assignToPattern(sp->inner, rest, env);
                    vi = rhs->array_value.size();
                } else {
                    auto val = (vi < rhs->array_value.size() ? rhs->array_value[vi] : Value::make_nil());
                    assignToPattern(el, val, env);
                    ++vi;
                }
            }
            return;
        }
        if(auto m = dynamic_pointer_cast<MapExpr>(pattern)){
            if(rhs->kind != ValueKind::MAP) return;
            for(auto &p: m->pairs){
                auto it = rhs->map_value.find(p.first);
                auto val = (it!=rhs->map_value.end()) ? it->second : Value::make_nil();
                assignToPattern(p.second, val, env);
            }
            return;
        }
        
    }
};


string readMultiline(){
    string all, line; int braces=0, paren=0, brack=0;
    cout << ">>> ";
    while(getline(cin, line)){
        all += line + "\n";
        for(char c: line){ if(c=='{') ++braces; if(c=='}') --braces; if(c=='(') ++paren; if(c==')') --paren; if(c=='[') ++brack; if(c==']') --brack; }
        if(braces<=0 && paren<=0 && brack<=0) break;
        cout << "... ";
    }
    return all;
}

int main(int argc, char** argv){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Interpreter interp;

    if(argc>1){
        ifstream in(argv[1]);
        if(in){
            string src((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
            Scanner sc(src); auto toks = sc.scanTokens();
            Parser p(toks); auto stmts = p.parse();
            for(auto &s: stmts) interp.executeStmt(s, interp.globals);
        }
    }

    cout << "SimpleMing REPL — type 'exit()' to quit.\n";
    while(true){
        string src = readMultiline();
        if(src.empty()) continue;
        Scanner sc(src); auto toks = sc.scanTokens();
        Parser p(toks); auto stmts = p.parse();
        try {
            for(auto &s: stmts){
                auto res = interp.executeStmt(s, interp.globals);
                if(dynamic_pointer_cast<ExprStmt>(s)) cout << res->toString() << "\n";
            }
        } catch(exception &e){
            cerr << "Error: " << e.what() << "\n";
        }
    }

    return 0;
}