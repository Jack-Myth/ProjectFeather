#include <Feather/Compiler.hpp>

#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Feather {
namespace {

enum class TokenKind {
    End, Name, Number, String, Def, Var, Export, Return, If, Else, While, Null,
    True, False, Object, LeftParen, RightParen, LeftBrace, RightBrace,
    LeftBracket, RightBracket, Comma, Dot, Semicolon, Assign, Equal,
    Less, Plus, Minus, Star, Slash, QuickHash, QuickDollar,
    QuickAmpersand, QuickRightAngle
};
struct Token {
    TokenKind Kind;
    std::string Text;
    std::size_t Offset;
    std::size_t Line;
    std::size_t Column;
};
[[noreturn]] void Fail(const Token& At, const std::string& Message) {
    throw std::runtime_error("source byte " + std::to_string(At.Offset) + " (" +
        std::to_string(At.Line) + ":" + std::to_string(At.Column) + "): " + Message);
}
class DepthGuard {
public:
    DepthGuard(std::size_t& Depth, const Token& At) : Depth(Depth) {
        if (++Depth > 256) Fail(At, "source nesting limit exceeded");
    }
    ~DepthGuard() { --Depth; }
private:
    std::size_t& Depth;
};
bool Alpha(char C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_'; }
bool Digit(char C) { return C >= '0' && C <= '9'; }
std::optional<std::size_t> InvalidUtf8(std::string_view Input) {
    for (std::size_t I = 0; I < Input.size();) {
        auto Lead = static_cast<unsigned char>(Input[I]);
        if (Lead < 0x80) { ++I; continue; }
        std::size_t Width = Lead >= 0xC2 && Lead <= 0xDF ? 2 :
                            Lead >= 0xE0 && Lead <= 0xEF ? 3 :
                            Lead >= 0xF0 && Lead <= 0xF4 ? 4 : 0;
        if (Width == 0 || I + Width > Input.size()) return I;
        for (std::size_t J = 1; J < Width; ++J) {
            auto Byte = static_cast<unsigned char>(Input[I + J]);
            if (Byte < 0x80 || Byte > 0xBF) return I + J;
        }
        auto Second = static_cast<unsigned char>(Input[I + 1]);
        if ((Lead == 0xE0 && Second < 0xA0) || (Lead == 0xED && Second > 0x9F) ||
            (Lead == 0xF0 && Second < 0x90) || (Lead == 0xF4 && Second > 0x8F)) return I;
        I += Width;
    }
    return std::nullopt;
}

class Lexer {
public:
    explicit Lexer(std::string_view Input) : Input(Input) {}
    std::vector<Token> Scan() {
        if (auto Invalid = InvalidUtf8(Input)) {
            Token At{TokenKind::End, {}, *Invalid, 1, 1};
            for (std::size_t I = 0; I < *Invalid; ++I) {
                if (Input[I] == '\n') { ++At.Line; At.Column = 1; }
                else ++At.Column;
            }
            Fail(At, "invalid UTF-8 source");
        }
        std::vector<Token> Output;
        bool AtLineStart = true;
        while (Position < Input.size()) {
            char C = Input[Position];
            if (C == ' ' || C == '\t' || C == '\r' || C == '\n') {
                if (C == '\n') AtLineStart = true;
                Advance(); continue;
            }
            if (C == '/' && Position + 1 < Input.size() && Input[Position + 1] == '/') {
                while (Position < Input.size() && Input[Position] != '\n') Advance();
                continue;
            }
            Token Start{TokenKind::End, {}, Position, Line, Column};
            if (AtLineStart && (C == '#' || C == '$' || C == '&' || C == '>')) {
                Start.Kind = C == '#' ? TokenKind::QuickHash :
                             C == '$' ? TokenKind::QuickDollar :
                             C == '&' ? TokenKind::QuickAmpersand : TokenKind::QuickRightAngle;
                Advance();
                while (Position < Input.size() && Input[Position] != '\n' && Input[Position] != '\r') {
                    if (Input[Position] == '\\' && Position + 2 < Input.size() &&
                        Input[Position + 1] == '/' && Input[Position + 2] == '/') {
                        Start.Text += "//";
                        Advance(); Advance(); Advance();
                    } else if (Input[Position] == '/' && Position + 1 < Input.size() &&
                               Input[Position + 1] == '/') break;
                    else { Start.Text += Input[Position]; Advance(); }
                }
                while (!Start.Text.empty() && (Start.Text.back() == ' ' || Start.Text.back() == '\t'))
                    Start.Text.pop_back();
                Output.push_back(std::move(Start));
                AtLineStart = false;
                continue;
            }
            if (Alpha(C)) {
                std::size_t Begin = Position;
                do { Advance(); } while (Position < Input.size() && (Alpha(Input[Position]) || Digit(Input[Position])));
                Start.Text = std::string(Input.substr(Begin, Position - Begin));
                static const std::unordered_map<std::string, TokenKind> Keywords{
                    {"def", TokenKind::Def}, {"var", TokenKind::Var},
                    {"export", TokenKind::Export}, {"return", TokenKind::Return},
                    {"if", TokenKind::If}, {"else", TokenKind::Else}, {"while", TokenKind::While},
                    {"null", TokenKind::Null}, {"true", TokenKind::True}, {"false", TokenKind::False},
                    {"object", TokenKind::Object}};
                auto Found = Keywords.find(Start.Text);
                Start.Kind = Found == Keywords.end() ? TokenKind::Name : Found->second;
            } else if (Digit(C)) {
                std::size_t Begin = Position;
                do { Advance(); } while (Position < Input.size() && Digit(Input[Position]));
                if (Position < Input.size() && Input[Position] == '.') {
                    if (Position + 1 < Input.size() && Digit(Input[Position + 1])) {
                        Advance();
                        do { Advance(); } while (Position < Input.size() && Digit(Input[Position]));
                    }
                }
                if (Position < Input.size() && (Input[Position] == 'e' || Input[Position] == 'E')) {
                    Advance();
                    if (Position < Input.size() && (Input[Position] == '+' || Input[Position] == '-')) Advance();
                    if (Position == Input.size() || !Digit(Input[Position])) Fail(Start, "invalid number exponent");
                    do { Advance(); } while (Position < Input.size() && Digit(Input[Position]));
                }
                Start.Kind = TokenKind::Number;
                Start.Text = std::string(Input.substr(Begin, Position - Begin));
            } else if (C == '"') {
                Advance();
                Start.Kind = TokenKind::String;
                bool Closed = false;
                while (Position < Input.size()) {
                    C = Input[Position];
                    if (C == '"') { Advance(); Closed = true; break; }
                    if (C == '\n' || C == '\r') Fail(Start, "newline in string");
                    if (C == '\\') {
                        Advance();
                        if (Position == Input.size()) Fail(Start, "unterminated escape");
                        switch (Input[Position]) {
                        case '\\': Start.Text += '\\'; break;
                        case '"': Start.Text += '"'; break;
                        case 'n': Start.Text += '\n'; break;
                        case 'r': Start.Text += '\r'; break;
                        case 't': Start.Text += '\t'; break;
                        default: Fail(Start, "invalid string escape");
                        }
                    } else Start.Text += C;
                    Advance();
                }
                if (!Closed) Fail(Start, "unterminated string");
                try { (void)Value::String(Start.Text); }
                catch (const std::exception&) { Fail(Start, "invalid UTF-8 string"); }
            } else {
                Advance();
                switch (C) {
                case '(': Start.Kind = TokenKind::LeftParen; break;
                case ')': Start.Kind = TokenKind::RightParen; break;
                case '{': Start.Kind = TokenKind::LeftBrace; break;
                case '}': Start.Kind = TokenKind::RightBrace; break;
                case '[': Start.Kind = TokenKind::LeftBracket; break;
                case ']': Start.Kind = TokenKind::RightBracket; break;
                case ',': Start.Kind = TokenKind::Comma; break;
                case '.': Start.Kind = TokenKind::Dot; break;
                case ';': Start.Kind = TokenKind::Semicolon; break;
                case '<': Start.Kind = TokenKind::Less; break;
                case '+': Start.Kind = TokenKind::Plus; break;
                case '-': Start.Kind = TokenKind::Minus; break;
                case '*': Start.Kind = TokenKind::Star; break;
                case '/': Start.Kind = TokenKind::Slash; break;
                case '=':
                    if (Position < Input.size() && Input[Position] == '=') { Advance(); Start.Kind = TokenKind::Equal; }
                    else Start.Kind = TokenKind::Assign;
                    break;
                default: Fail(Start, "unexpected character");
                }
            }
            Output.push_back(std::move(Start));
            AtLineStart = false;
        }
        Output.push_back({TokenKind::End, {}, Position, Line, Column});
        return Output;
    }
private:
    void Advance() {
        if (Input[Position++] == '\n') { ++Line; Column = 1; }
        else ++Column;
    }
    std::string_view Input;
    std::size_t Position = 0, Line = 1, Column = 1;
};

enum class NodeKind { Literal, Name, Object, Member, Call, Unary, Binary, Assign,
    Var, Return, If, While, Block, Expression };
struct Node {
    NodeKind Kind;
    Token At;
    std::string Name;
    Value Literal;
    TokenKind Operator = TokenKind::End;
    std::vector<std::unique_ptr<Node>> Children;
};
using NodePtr = std::unique_ptr<Node>;
NodePtr Make(NodeKind Kind, const Token& At) {
    auto Result = std::make_unique<Node>(); Result->Kind = Kind; Result->At = At; return Result;
}
struct FunctionAst {
    Token At;
    std::string Name;
    std::vector<std::string> Parameters;
    std::vector<std::optional<Value>> Defaults;
    NodePtr Body;
};
struct ProgramAst {
    std::vector<FunctionAst> Functions;
    std::vector<NodePtr> Statements;
    std::vector<std::string> Exports;
};

class Parser {
public:
    explicit Parser(std::vector<Token> Tokens) : Tokens(std::move(Tokens)) {}
    ProgramAst Parse() {
        ProgramAst Result;
        std::unordered_set<std::string> Declared;
        while (!Check(TokenKind::End)) {
            bool Exported = Match(TokenKind::Export);
            if (Match(TokenKind::Def)) {
                auto Function = ParseFunction(Previous());
                if (!Declared.insert(Function.Name).second) Fail(Function.At, "duplicate global declaration");
                if (Exported) Result.Exports.push_back(Function.Name);
                Result.Functions.push_back(std::move(Function));
            } else {
                if (Exported && !Check(TokenKind::Var))
                    Fail(Peek(), "export requires a top-level var or def declaration");
                auto Statement = ParseStatement();
                if (Statement->Kind == NodeKind::Var && !Declared.insert(Statement->Name).second)
                    Fail(Statement->At, "duplicate global declaration");
                if (Exported) Result.Exports.push_back(Statement->Name);
                Result.Statements.push_back(std::move(Statement));
            }
        }
        return Result;
    }
private:
    const Token& Peek() const { return Tokens[Position]; }
    const Token& Previous() const { return Tokens[Position - 1]; }
    bool Check(TokenKind Kind) const { return Peek().Kind == Kind; }
    bool Match(TokenKind Kind) { if (!Check(Kind)) return false; ++Position; return true; }
    Token Expect(TokenKind Kind, const char* Message) {
        if (!Check(Kind)) Fail(Peek(), Message);
        return Tokens[Position++];
    }
    Value ParseLiteral() {
        Token Item = Tokens[Position++];
        switch (Item.Kind) {
        case TokenKind::Null: return {};
        case TokenKind::True: return Value::Bool(true);
        case TokenKind::False: return Value::Bool(false);
        case TokenKind::String: return Value::String(Item.Text);
        case TokenKind::Number: {
            double Number = 0;
            auto Result = std::from_chars(Item.Text.data(), Item.Text.data() + Item.Text.size(), Number);
            if (Result.ec != std::errc{} || Result.ptr != Item.Text.data() + Item.Text.size() ||
                !std::isfinite(Number)) Fail(Item, "number out of range");
            return Value::Number(Number);
        }
        default: Fail(Item, "expected literal");
        }
    }
    FunctionAst ParseFunction(const Token& At) {
        FunctionAst Result; Result.At = At;
        Result.Name = Expect(TokenKind::Name, "expected function name").Text;
        Expect(TokenKind::LeftParen, "expected '('");
        std::unordered_set<std::string> Names;
        bool SawDefault = false;
        if (!Check(TokenKind::RightParen)) do {
            auto Parameter = Expect(TokenKind::Name, "expected parameter name");
            if (!Names.insert(Parameter.Text).second) Fail(Parameter, "duplicate parameter");
            Result.Parameters.push_back(Parameter.Text);
            if (Match(TokenKind::Assign)) { SawDefault = true; Result.Defaults.push_back(ParseLiteral()); }
            else { if (SawDefault) Fail(Parameter, "required parameter after default"); Result.Defaults.push_back(std::nullopt); }
        } while (Match(TokenKind::Comma));
        Expect(TokenKind::RightParen, "expected ')'");
        Result.Body = ParseBlock();
        return Result;
    }
    NodePtr ParseBlock() {
        auto At = Expect(TokenKind::LeftBrace, "expected '{'");
        auto Result = Make(NodeKind::Block, At);
        while (!Check(TokenKind::RightBrace)) {
            if (Check(TokenKind::End)) Fail(Peek(), "unterminated block");
            if (Check(TokenKind::Def)) Fail(Peek(), "function declarations are top-level only");
            if (Check(TokenKind::Export)) Fail(Peek(), "export declarations are top-level only");
            Result->Children.push_back(ParseStatement());
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return Result;
    }
    NodePtr ParseStatement() {
        DepthGuard Guard(Depth, Peek());
        if (Check(TokenKind::LeftBrace)) return ParseBlock();
        if (Match(TokenKind::QuickHash) || Match(TokenKind::QuickDollar) ||
            Match(TokenKind::QuickAmpersand) || Match(TokenKind::QuickRightAngle))
            return ParseQuickLine(Previous());
        if (Match(TokenKind::Var)) {
            auto Result = Make(NodeKind::Var, Previous());
            Result->Name = Expect(TokenKind::Name, "expected variable name").Text;
            if (Match(TokenKind::Assign)) Result->Children.push_back(ParseExpression());
            Expect(TokenKind::Semicolon, "expected ';'"); return Result;
        }
        if (Match(TokenKind::Return)) {
            auto Result = Make(NodeKind::Return, Previous());
            if (!Check(TokenKind::Semicolon)) Result->Children.push_back(ParseExpression());
            Expect(TokenKind::Semicolon, "expected ';'"); return Result;
        }
        if (Match(TokenKind::If)) {
            auto Result = Make(NodeKind::If, Previous());
            Expect(TokenKind::LeftParen, "expected '('");
            Result->Children.push_back(ParseExpression());
            Expect(TokenKind::RightParen, "expected ')'");
            Result->Children.push_back(ParseBlock());
            if (Match(TokenKind::Else)) Result->Children.push_back(ParseBlock());
            return Result;
        }
        if (Match(TokenKind::While)) {
            auto Result = Make(NodeKind::While, Previous());
            Expect(TokenKind::LeftParen, "expected '('");
            Result->Children.push_back(ParseExpression());
            Expect(TokenKind::RightParen, "expected ')'");
            Result->Children.push_back(ParseBlock()); return Result;
        }
        auto Result = Make(NodeKind::Expression, Peek());
        Result->Children.push_back(ParseExpression());
        Expect(TokenKind::Semicolon, "expected ';'"); return Result;
    }
    NodePtr ParseQuickLine(const Token& At) {
        const char* Function = nullptr;
        switch (At.Kind) {
        case TokenKind::QuickHash: Function = "__QuickOperatorHash"; break;
        case TokenKind::QuickDollar: Function = "__QuickOperatorDollar"; break;
        case TokenKind::QuickAmpersand: Function = "__QuickOperatorAmpersand"; break;
        case TokenKind::QuickRightAngle: Function = "__QuickOperatorRightAngleBucket"; break;
        default: Fail(At, "internal invalid quick operator");
        }
        auto Statement = Make(NodeKind::Expression, At);
        auto Call = Make(NodeKind::Call, At);
        auto Target = Make(NodeKind::Name, At);
        Target->Name = Function;
        auto Argument = Make(NodeKind::Literal, At);
        Argument->Literal = Value::String(At.Text);
        Call->Children.push_back(std::move(Target));
        Call->Children.push_back(std::move(Argument));
        Statement->Children.push_back(std::move(Call));
        return Statement;
    }
    NodePtr ParseExpression() { DepthGuard Guard(Depth, Peek()); return ParseAssignment(); }
    NodePtr ParseAssignment() {
        DepthGuard Guard(Depth, Peek());
        auto Left = ParseEquality();
        if (!Match(TokenKind::Assign)) return Left;
        auto At = Previous();
        if (Left->Kind != NodeKind::Name && Left->Kind != NodeKind::Member)
            Fail(At, "invalid assignment target");
        auto Result = Make(NodeKind::Assign, At);
        Result->Children.push_back(std::move(Left));
        Result->Children.push_back(ParseAssignment()); return Result;
    }
    NodePtr ParseEquality() { return ParseBinary(&Parser::ParseComparison, {TokenKind::Equal}); }
    NodePtr ParseComparison() { return ParseBinary(&Parser::ParseTerm, {TokenKind::Less}); }
    NodePtr ParseTerm() { return ParseBinary(&Parser::ParseFactor, {TokenKind::Plus, TokenKind::Minus}); }
    NodePtr ParseFactor() { return ParseBinary(&Parser::ParseUnary, {TokenKind::Star, TokenKind::Slash}); }
    NodePtr ParseBinary(NodePtr (Parser::*Operand)(), std::initializer_list<TokenKind> Operators) {
        auto Left = (this->*Operand)();
        while (true) {
            bool Found = false;
            for (auto Kind : Operators) if (Match(Kind)) { Found = true; break; }
            if (!Found) return Left;
            auto At = Previous();
            auto Result = Make(NodeKind::Binary, At); Result->Operator = At.Kind;
            Result->Children.push_back(std::move(Left));
            Result->Children.push_back((this->*Operand)());
            Left = std::move(Result);
        }
    }
    NodePtr ParseUnary() {
        if (!Match(TokenKind::Minus)) return ParsePostfix();
        DepthGuard Guard(Depth, Previous());
        auto Result = Make(NodeKind::Unary, Previous());
        Result->Children.push_back(ParseUnary()); return Result;
    }
    NodePtr ParsePostfix() {
        auto Left = ParsePrimary();
        while (true) {
            if (Match(TokenKind::LeftParen)) {
                auto Result = Make(NodeKind::Call, Previous());
                Result->Children.push_back(std::move(Left));
                if (!Check(TokenKind::RightParen)) do {
                    Result->Children.push_back(ParseExpression());
                } while (Match(TokenKind::Comma));
                Expect(TokenKind::RightParen, "expected ')'"); Left = std::move(Result);
            } else if (Match(TokenKind::Dot)) {
                auto Result = Make(NodeKind::Member, Previous());
                auto Name = Expect(TokenKind::Name, "expected member name");
                auto Key = Make(NodeKind::Literal, Name); Key->Literal = Value::String(Name.Text);
                Result->Children.push_back(std::move(Left)); Result->Children.push_back(std::move(Key));
                Left = std::move(Result);
            } else if (Match(TokenKind::LeftBracket)) {
                auto Result = Make(NodeKind::Member, Previous());
                Result->Children.push_back(std::move(Left));
                Result->Children.push_back(ParseExpression());
                Expect(TokenKind::RightBracket, "expected ']'"); Left = std::move(Result);
            } else return Left;
        }
    }
    NodePtr ParsePrimary() {
        Token At = Peek();
        if (Match(TokenKind::Null) || Match(TokenKind::True) || Match(TokenKind::False) ||
            Match(TokenKind::Number) || Match(TokenKind::String)) {
            --Position;
            auto Result = Make(NodeKind::Literal, At); Result->Literal = ParseLiteral(); return Result;
        }
        if (Match(TokenKind::Name)) {
            auto Result = Make(NodeKind::Name, At); Result->Name = At.Text; return Result;
        }
        if (Match(TokenKind::Object)) {
            Expect(TokenKind::LeftParen, "expected '(' after object");
            Expect(TokenKind::RightParen, "object() takes no arguments");
            return Make(NodeKind::Object, At);
        }
        if (Match(TokenKind::LeftParen)) {
            auto Result = ParseExpression(); Expect(TokenKind::RightParen, "expected ')'"); return Result;
        }
        Fail(At, "expected expression");
    }
    std::vector<Token> Tokens;
    std::size_t Position = 0;
    std::size_t Depth = 0;
};

class Emitter {
public:
    Emitter(Module& Program, const Token& At) : Program(Program), At(At), CurrentAt(&this->At) {
        Scopes.emplace_back();
        ScopeSymbols.emplace_back();
    }
    void AddParameters(const FunctionAst& Function) {
        if (Function.Parameters.size() > 65'536) Fail(Function.At, "too many parameters");
        for (const auto& Name : Function.Parameters) {
            auto Slot = LocalCount++;
            Scopes.back()[Name] = Slot;
            ScopeSymbols.back().push_back(LocalVariables.size());
            LocalVariables.push_back({Name, Slot, 0, std::numeric_limits<std::size_t>::max()});
        }
    }
    void EmitFunctionBody(const FunctionAst& Function) {
        for (const auto& Statement : Function.Body->Children) EmitStatement(*Statement, false);
        BreakableGuard Synthetic(CurrentBreakable, false);
        Emit(Op::Null); Emit(Op::Return);
    }
    void EmitInitializer(const ProgramAst& Ast,
                         const std::unordered_map<std::string, std::uint32_t>& Functions) {
        for (const auto& Function : Ast.Functions) {
            SourceGuard Source(CurrentAt, Function.At);
            BreakableGuard Synthetic(CurrentBreakable, false);
            EmitU32(Op::Const, Functions.at(Function.Name));
            EmitU32(Op::SetGlobal, StringConstant(Function.Name));
            Emit(Op::Pop);
        }
        for (const auto& Statement : Ast.Statements) EmitStatement(*Statement, true);
        BreakableGuard Synthetic(CurrentBreakable, false);
        Emit(Op::Null); Emit(Op::Return);
    }
    std::vector<std::uint8_t> Finish() { return std::move(Code).Finish(); }
    std::vector<InstructionLocation> FinishLocations() { return std::move(Locations); }
    std::vector<LocalVariableInfo> FinishLocalVariables() {
        auto End = Code.Offset();
        for (auto& Variable : LocalVariables)
            if (Variable.EndPc == std::numeric_limits<std::size_t>::max()) Variable.EndPc = End;
        return std::move(LocalVariables);
    }
    std::uint32_t GetLocalCount() const { return LocalCount; }
private:
    struct SourceGuard {
        SourceGuard(const Token*& Current, const Token& At) : Current(Current), Previous(Current) { Current = &At; }
        ~SourceGuard() { Current = Previous; }
        const Token*& Current;
        const Token* Previous;
    };
    struct BreakableGuard {
        BreakableGuard(bool& Current, bool Value) : Current(Current), Previous(Current) {
            Current = Value;
        }
        ~BreakableGuard() { Current = Previous; }
        bool& Current;
        bool Previous;
    };
    void RecordLocation() {
        Locations.push_back({Code.Offset(),
            {CurrentAt->Offset, CurrentAt->Line, CurrentAt->Column}, CurrentBreakable});
    }
    std::uint32_t ConstantFor(const Value& Input, const Token& At) {
        if (Program.Constants.size() >= 65'536) Fail(At, "too many constants");
        if (Input.GetType() == ValueType::Number) return Program.AddNumber(Input.AsNumber());
        if (Input.GetType() == ValueType::String) return Program.AddString(Input.AsString());
        Fail(At, "internal nonconstant literal");
    }
    std::uint32_t StringConstant(const std::string& Input) {
        if (Program.Constants.size() >= 65'536) Fail(At, "too many constants");
        return Program.AddString(Input);
    }
    std::optional<std::uint32_t> FindLocal(const std::string& Name) const {
        for (auto Scope = Scopes.rbegin(); Scope != Scopes.rend(); ++Scope) {
            auto Found = Scope->find(Name);
            if (Found != Scope->end()) return Found->second;
        }
        return std::nullopt;
    }
    void Emit(Op Instruction) { RecordLocation(); Code.Emit(Instruction); }
    void EmitU16(Op Instruction, std::uint16_t Operand) { RecordLocation(); Code.EmitU16(Instruction, Operand); }
    void EmitU32(Op Instruction, std::uint32_t Operand) { RecordLocation(); Code.EmitU32(Instruction, Operand); }
    std::size_t EmitJump(Op Instruction) { RecordLocation(); return Code.EmitJump(Instruction); }
    void EmitBlock(const Node& Block, bool NewScope) {
        DepthGuard Guard(Depth, Block.At);
        if (NewScope) {
            Scopes.emplace_back();
            ScopeSymbols.emplace_back();
        }
        for (const auto& Statement : Block.Children) EmitStatement(*Statement, false);
        if (NewScope) {
            for (auto Index : ScopeSymbols.back()) LocalVariables[Index].EndPc = Code.Offset();
            ScopeSymbols.pop_back();
            Scopes.pop_back();
        }
    }
    void EmitStatement(const Node& Statement, bool TopLevel) {
        DepthGuard Guard(Depth, Statement.At);
        SourceGuard Source(CurrentAt, Statement.At);
        switch (Statement.Kind) {
        case NodeKind::Var: {
            if (!TopLevel && Scopes.back().contains(Statement.Name))
                Fail(Statement.At, "duplicate local declaration");
            if (Statement.Children.empty()) Emit(Op::Null);
            else EmitExpression(*Statement.Children[0]);
            if (TopLevel) EmitU32(Op::SetGlobal, StringConstant(Statement.Name));
            else {
                if (LocalCount >= 65'536) Fail(Statement.At, "too many local slots");
                auto Slot = LocalCount++;
                Scopes.back()[Statement.Name] = Slot;
                EmitU32(Op::SetLocal, Slot);
                ScopeSymbols.back().push_back(LocalVariables.size());
                LocalVariables.push_back({Statement.Name, Slot, Code.Offset(),
                    std::numeric_limits<std::size_t>::max()});
            }
            Emit(Op::Pop); break;
        }
        case NodeKind::Return:
            if (Statement.Children.empty()) Emit(Op::Null);
            else EmitExpression(*Statement.Children[0]);
            Emit(Op::Return); break;
        case NodeKind::Expression:
            EmitExpression(*Statement.Children[0]); Emit(Op::Pop); break;
        case NodeKind::Block:
            EmitBlock(Statement, true); break;
        case NodeKind::If: {
            EmitExpression(*Statement.Children[0]);
            auto True = EmitJump(Op::JumpIf);
            auto False = EmitJump(Op::Jump);
            Code.PatchJump(True, Code.Offset());
            EmitBlock(*Statement.Children[1], true);
            if (Statement.Children.size() == 3) {
                auto End = EmitJump(Op::Jump);
                Code.PatchJump(False, Code.Offset());
                EmitBlock(*Statement.Children[2], true);
                Code.PatchJump(End, Code.Offset());
            } else Code.PatchJump(False, Code.Offset());
            break;
        }
        case NodeKind::While: {
            auto Start = Code.Offset();
            EmitExpression(*Statement.Children[0]);
            auto Body = EmitJump(Op::JumpIf);
            auto Exit = EmitJump(Op::Jump);
            Code.PatchJump(Body, Code.Offset());
            EmitBlock(*Statement.Children[1], true);
            auto Back = EmitJump(Op::Jump);
            Code.PatchJump(Back, Start);
            Code.PatchJump(Exit, Code.Offset()); break;
        }
        default: Fail(Statement.At, "internal invalid statement");
        }
    }
    void EmitExpression(const Node& Expression) {
        DepthGuard Guard(Depth, Expression.At);
        SourceGuard Source(CurrentAt, Expression.At);
        switch (Expression.Kind) {
        case NodeKind::Literal:
            switch (Expression.Literal.GetType()) {
            case ValueType::Null: Emit(Op::Null); break;
            case ValueType::Bool: Emit(Expression.Literal.AsBool() ? Op::True : Op::False); break;
            case ValueType::Number: case ValueType::String:
                EmitU32(Op::Const, ConstantFor(Expression.Literal, Expression.At)); break;
            default: Fail(Expression.At, "invalid literal");
            }
            break;
        case NodeKind::Name: {
            auto Local = FindLocal(Expression.Name);
            if (Local) EmitU32(Op::GetLocal, *Local);
            else EmitU32(Op::GetGlobal, StringConstant(Expression.Name));
            break;
        }
        case NodeKind::Object: Emit(Op::NewObject); break;
        case NodeKind::Member:
            EmitExpression(*Expression.Children[0]);
            EmitExpression(*Expression.Children[1]);
            Emit(Op::GetMember); break;
        case NodeKind::Call: {
            if (Expression.Children.size() - 1 > std::numeric_limits<std::uint16_t>::max())
                Fail(Expression.At, "too many call arguments");
            for (const auto& Child : Expression.Children) EmitExpression(*Child);
            EmitU16(Op::Call, static_cast<std::uint16_t>(Expression.Children.size() - 1));
            break;
        }
        case NodeKind::Unary:
            EmitExpression(*Expression.Children[0]); Emit(Op::Negate); break;
        case NodeKind::Binary: {
            EmitExpression(*Expression.Children[0]);
            EmitExpression(*Expression.Children[1]);
            switch (Expression.Operator) {
            case TokenKind::Plus: Emit(Op::Add); break;
            case TokenKind::Minus: Emit(Op::Sub); break;
            case TokenKind::Star: Emit(Op::Mul); break;
            case TokenKind::Slash: Emit(Op::Div); break;
            case TokenKind::Equal: Emit(Op::Equal); break;
            case TokenKind::Less: Emit(Op::Less); break;
            default: Fail(Expression.At, "internal invalid binary operator");
            }
            break;
        }
        case NodeKind::Assign: {
            const auto& Target = *Expression.Children[0];
            if (Target.Kind == NodeKind::Name) {
                EmitExpression(*Expression.Children[1]);
                auto Local = FindLocal(Target.Name);
                if (Local) EmitU32(Op::SetLocal, *Local);
                else EmitU32(Op::SetGlobal, StringConstant(Target.Name));
            } else {
                EmitExpression(*Target.Children[0]);
                EmitExpression(*Target.Children[1]);
                EmitExpression(*Expression.Children[1]);
                Emit(Op::SetMember);
            }
            break;
        }
        default: Fail(Expression.At, "internal invalid expression");
        }
    }
    Module& Program;
    Token At;
    const Token* CurrentAt;
    bool CurrentBreakable = true;
    Builder Code;
    std::vector<InstructionLocation> Locations;
    std::uint32_t LocalCount = 0;
    std::size_t Depth = 0;
    std::vector<std::unordered_map<std::string, std::uint32_t>> Scopes;
    std::vector<std::vector<std::size_t>> ScopeSymbols;
    std::vector<LocalVariableInfo> LocalVariables;
};

} // namespace

CompiledProgram Compile(std::string_view Source) {
    auto Tokens = Lexer(Source).Scan();
    bool UsesQuickOperators = false;
    for (const auto& Token : Tokens)
        if (Token.Kind == TokenKind::QuickHash || Token.Kind == TokenKind::QuickDollar ||
            Token.Kind == TokenKind::QuickAmpersand || Token.Kind == TokenKind::QuickRightAngle)
            UsesQuickOperators = true;
    auto Ast = Parser(std::move(Tokens)).Parse();
    CompiledProgram Result;
    Result.UsesQuickOperators = UsesQuickOperators;
    Result.Exports = std::move(Ast.Exports);
    Result.Program = std::make_shared<Module>();
    for (const auto& Function : Ast.Functions) {
        auto Prototype = std::make_shared<FunctionPrototype>();
        Prototype->ParameterCount = static_cast<std::uint32_t>(Function.Parameters.size());
        Prototype->Defaults = Function.Defaults;
        Result.Functions.emplace(Function.Name, Result.Program->AddFunction(std::move(Prototype)));
    }
    auto Initializer = std::make_shared<FunctionPrototype>();
    Result.Initializer = Result.Program->AddFunction(Initializer);
    for (const auto& Function : Ast.Functions) {
        Emitter Output(*Result.Program, Function.At);
        Output.AddParameters(Function);
        Output.EmitFunctionBody(Function);
        auto& Prototype = *Result.Program->Constants[Result.Functions.at(Function.Name)].Function;
        Prototype.DebugName = Function.Name;
        Prototype.LocalCount = Output.GetLocalCount();
        Prototype.LocalVariables = Output.FinishLocalVariables();
        Prototype.Code = Output.Finish();
        Prototype.Locations = Output.FinishLocations();
    }
    Token Start{TokenKind::End, {}, 0, 1, 1};
    Emitter Output(*Result.Program, Start);
    Output.EmitInitializer(Ast, Result.Functions);
    Initializer->DebugName = "<initializer>";
    Initializer->LocalCount = Output.GetLocalCount();
    Initializer->LocalVariables = Output.FinishLocalVariables();
    Initializer->Code = Output.Finish();
    Initializer->Locations = Output.FinishLocations();
    try { Result.Program->Validate(); }
    catch (const std::exception& Error) { Fail(Start, std::string("generated bytecode invalid: ") + Error.what()); }
    return Result;
}

} // namespace Feather
