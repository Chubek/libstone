{ Standalone acsem @VERSION@ interpreter, Free Pascal 3.2+. MIT-0. }
program AcsemProgram;
{$mode objfpc}{$H+}{$B-}
uses SysUtils, Classes;

const
  ModelData: AnsiString =
@MODEL@;
  MinInteger = Int64(-2147483648);
  MaxInteger = Int64(2147483647);
type
  TValue = record Kind: LongInt; N: Int64 end;
  TBinding = record Key: LongInt; Value: TValue end;
  TValues = record Count: LongInt; Items: array[0..63] of TValue end;
  TEnv = record Count: LongInt; Items: array[0..63] of TBinding end;
  TOutcome = record Status: LongInt; OutValues: TValues; Bindings: TEnv end;
  TConstructor = record
    Name: ShortString;
    Sort, Count, Rule: LongInt;
    Fields: array[0..15] of LongInt;
  end;
  TInstruction = record Op, X, Y, N: LongInt; Text: ShortString end;
  TTerm = record Ctor: LongInt; Fields: array[0..15] of TValue end;
  TToken = record Text: ShortString; Kind, At: LongInt end;
  EAcsem = class(Exception);
var
  Ctors: array[0..255] of TConstructor;
  Code: array[0..4095] of TInstruction;
  Terms: array[0..2047] of TTerm;
  Tokens: array[0..20000] of TToken;
  Cells: array[0..1023] of TValue;
  Symbols: array[0..255] of ShortString;
  Source: RawByteString;
  StartSort, CtorCount, TermCount, TokenCount, TokenPos: LongInt;
  CellCount, SymbolCount, Steps, ModelPos: LongInt;

procedure Error(const MessageText: AnsiString);
begin raise EAcsem.Create('ACSEM_ERROR: ' + MessageText) end;
procedure Limit(const MessageText: AnsiString);
begin raise EAcsem.Create('ACSEM_LIMIT: ' + MessageText) end;
procedure SyntaxAt(const MessageText: AnsiString; At: LongInt);
var I, Line, Col: LongInt;
begin
  Line := 1; Col := 1;
  for I := 1 to At - 1 do
    if Source[I] = #10 then begin Inc(Line); Col := 1 end else Inc(Col);
  Error(MessageText + ' at ' + IntToStr(Line) + ':' + IntToStr(Col));
end;
procedure Syntax(const MessageText: AnsiString);
begin SyntaxAt(MessageText, Tokens[TokenPos].At) end;
function Letter(C: AnsiChar): Boolean;
begin Result := C in ['A'..'Z', 'a'..'z', '_'] end;
function Digit(C: AnsiChar): Boolean;
begin Result := C in ['0'..'9'] end;
function Value(Kind: LongInt; N: Int64): TValue;
begin Result.Kind := Kind; Result.N := N end;
function Symbol(const Text: ShortString): LongInt;
var I: LongInt;
begin
  for I := 0 to SymbolCount - 1 do if Symbols[I] = Text then Exit(I);
  if SymbolCount = 256 then Limit('runtime symbol limit');
  Symbols[SymbolCount] := Text; Result := SymbolCount; Inc(SymbolCount);
end;
function ModelWord: AnsiString;
var First: LongInt;
begin
  while (ModelPos <= Length(ModelData)) and (ModelData[ModelPos] = ' ') do Inc(ModelPos);
  First := ModelPos;
  while (ModelPos <= Length(ModelData)) and (ModelData[ModelPos] <> ' ') do Inc(ModelPos);
  Result := Copy(ModelData, First, ModelPos - First);
end;
function ModelNumber: LongInt;
begin Result := StrToInt(ModelWord) end;
procedure LoadModel;
var I, J, N: LongInt;
begin
  ModelPos := 1;
  StartSort := ModelNumber; CtorCount := ModelNumber; N := ModelNumber;
  for I := 0 to CtorCount - 1 do begin
    Ctors[I].Name := ModelWord; Ctors[I].Sort := ModelNumber;
    Ctors[I].Count := ModelNumber; Ctors[I].Rule := ModelNumber;
    for J := 0 to Ctors[I].Count - 1 do Ctors[I].Fields[J] := ModelNumber;
  end;
  for I := 0 to N - 1 do begin
    Code[I].Op := ModelNumber; Code[I].X := ModelNumber; Code[I].Y := ModelNumber;
    Code[I].N := ModelNumber; Code[I].Text := ModelWord;
  end;
end;
procedure Lex;
var At, First, Kind: LongInt; C: AnsiChar;
begin
  if Length(Source) > 65536 then Limit('source exceeds 64 KiB');
  At := 1;
  while At <= Length(Source) do begin
    C := Source[At]; First := At; Kind := 3;
    if C in [' ', #9, #10, #13] then begin Inc(At); Continue end;
    if Letter(C) then begin
      Kind := 1; Inc(At);
      while (At <= Length(Source)) and (Letter(Source[At]) or Digit(Source[At])) do Inc(At);
    end else if Digit(C) then begin
      Kind := 2; Inc(At);
      while (At <= Length(Source)) and Digit(Source[At]) do Inc(At);
    end else if C in ['{', '}', '(', ')', ',', ';', ':', '=', '|', '+', '*', '/', '-', '<'] then begin
      Inc(At);
      if (C = '=') and (At <= Length(Source)) and (Source[At] = '=') then Inc(At);
    end else if C = #0 then SyntaxAt('NUL in source', At) else SyntaxAt('invalid term syntax', At);
    if At - First > 64 then Limit('source token exceeds 64 bytes');
    if TokenCount = 20000 then Limit('too many tokens');
    Tokens[TokenCount].Text := Copy(Source, First, At - First);
    Tokens[TokenCount].Kind := Kind; Tokens[TokenCount].At := First; Inc(TokenCount);
  end;
  Tokens[TokenCount].Text := '<eof>'; Tokens[TokenCount].At := Length(Source) + 1;
end;
function Eat(const Text: ShortString): Boolean;
begin
  Result := Tokens[TokenPos].Text = Text;
  if Result then Inc(TokenPos);
end;
procedure Expect(const Text: ShortString);
begin if not Eat(Text) then Syntax('expected ''' + Text + '''') end;
function Ident: ShortString;
begin
  if Tokens[TokenPos].Kind <> 1 then Syntax('expected identifier');
  Result := Tokens[TokenPos].Text; Inc(TokenPos);
end;
function ParseTerm(Sort, Depth: LongInt): LongInt;
var Index, CI, I: LongInt; Name, Text: AnsiString; Negative: Boolean; N: Int64;
begin
  if Depth > 128 then Limit('parse depth exceeded');
  if TermCount = 2048 then Limit('program syntax node limit');
  Index := TermCount; Inc(TermCount); Name := Ident; CI := 0;
  while (CI < CtorCount) and (Ctors[CI].Name <> Name) do Inc(CI);
  if CI = CtorCount then Syntax('unknown constructor');
  if Ctors[CI].Sort <> Sort then Syntax('constructor has wrong sort');
  Terms[Index].Ctor := CI; Expect('(');
  for I := 0 to Ctors[CI].Count - 1 do begin
    if I <> 0 then Expect(',');
    if Ctors[CI].Fields[I] = -1 then begin
      Negative := Eat('-');
      if Tokens[TokenPos].Kind <> 2 then Syntax('integer field expected');
      Text := Tokens[TokenPos].Text; Inc(TokenPos);
      while (Length(Text) > 1) and (Text[1] = '0') do Delete(Text, 1, 1);
      if Negative then Text := '-' + Text;
      if not TryStrToInt64(Text, N) then Syntax('integer field outside 32-bit range');
      if (N < MinInteger) or (N > MaxInteger) then Syntax('integer field outside 32-bit range');
      Terms[Index].Fields[I] := Value(1, N);
    end else if Ctors[CI].Fields[I] = -2 then Terms[Index].Fields[I] := Value(2, Symbol(Ident))
    else Terms[Index].Fields[I] := Value(4, ParseTerm(Ctors[CI].Fields[I], Depth + 1));
  end;
  Expect(')'); Result := Index;
end;
procedure Tick(Depth: LongInt);
begin
  Inc(Steps);
  if Steps > 100000 then Limit('evaluation step budget exceeded');
  if Depth > 128 then Limit('evaluation nesting exceeded');
end;
function Lookup(const Env: TEnv; Key: LongInt): TValue;
var I: LongInt;
begin
  for I := 0 to Env.Count - 1 do if Env.Items[I].Key = Key then Exit(Env.Items[I].Value);
  Result := Value(0, 0);
end;
function Expr(PC, Term: LongInt; const Inputs: TValues; const Env: TEnv; Depth: LongInt): TValue;
var E: TInstruction; A, B: TValue; N: Int64;
begin
  Tick(Depth); E := Code[PC];
  case E.Op of
    1: Exit(Value(1, E.N));
    2: Exit(Value(2, Symbol(E.Text)));
    3: Exit(Terms[Term].Fields[E.N]);
    4: if E.N < Inputs.Count then Exit(Inputs.Items[E.N]) else Exit(Value(0, 0));
  end;
  A := Expr(E.X, Term, Inputs, Env, Depth + 1);
  if E.Op = 5 then begin
    if A.Kind = 2 then Exit(Lookup(Env, A.N)) else Exit(Value(0, 0));
  end;
  if E.Op = 6 then begin
    if (A.Kind = 3) and (A.N >= 0) and (A.N < CellCount) then Exit(Cells[A.N]) else Exit(Value(0, 0));
  end;
  B := Expr(E.Y, Term, Inputs, Env, Depth + 1);
  if (A.Kind <> 1) or (B.Kind <> 1) then Exit(Value(0, 0));
  N := 0;
  case E.Op of
    7: N := A.N + B.N;
    8: N := A.N - B.N;
    9: N := A.N * B.N;
    10: begin
      if (B.N = 0) or ((A.N = MinInteger) and (B.N = -1)) then Exit(Value(0, 0));
      N := A.N div B.N;
    end;
    11: N := Ord(A.N = B.N);
    12: N := Ord(A.N < B.N);
    else Error('invalid expression instruction');
  end;
  if (N < MinInteger) or (N > MaxInteger) then Result := Value(0, 0) else Result := Value(1, N);
end;
function Empty(Status: LongInt): TOutcome;
begin FillChar(Result, SizeOf(Result), 0); Result.Status := Status end;
function Merge(var Dest: TEnv; const A, B: TEnv; Overlay: Boolean): Boolean;
var I, J, Count: LongInt; Found: Boolean; V: TValue;
begin
  Count := A.Count;
  for I := 0 to B.Count - 1 do begin
    V := Lookup(A, B.Items[I].Key); Found := V.Kind <> 0;
    if Found and not Overlay then Exit(False);
    if not Found then Inc(Count);
  end;
  if Count > 64 then Limit('binding result capacity');
  Dest := A;
  for I := 0 to B.Count - 1 do begin
    J := 0;
    while (J < Dest.Count) and (Dest.Items[J].Key <> B.Items[I].Key) do Inc(J);
    if J = Dest.Count then Inc(Dest.Count);
    Dest.Items[J] := B.Items[I];
  end;
  Result := True;
end;
function RunNode(Term: LongInt; const Inputs: TValues; const Env: TEnv; Depth: LongInt): TOutcome; forward;
function Action(PC, Term: LongInt; const Inputs: TValues; const Env: TEnv; Depth: LongInt): TOutcome;
var A: TInstruction; Left, Right, R: TOutcome; V, W: TValue; I: LongInt;
begin
  Tick(Depth); A := Code[PC];
  if A.Op = 13 then Exit(Empty(0));
  if A.Op = 14 then Exit(Empty(1));
  if A.Op in [15, 16, 17] then begin
    V := Expr(A.X, Term, Inputs, Env, Depth + 1);
    if V.Kind = 0 then Exit(Empty(1));
    if A.Op = 16 then begin
      if (V.Kind = 1) and (V.N <> 0) then Exit(Empty(0)) else Exit(Empty(1));
    end;
    if A.Op = 17 then R := Empty(2) else R := Empty(0);
    R.OutValues.Count := 1; R.OutValues.Items[0] := V; Exit(R);
  end;
  if A.Op = 19 then begin
    if CellCount = 1024 then Limit('allocated cell limit');
    R := Empty(0); R.OutValues.Count := 1; R.OutValues.Items[0] := Value(3, CellCount);
    Inc(CellCount); Exit(R);
  end;
  if (A.Op = 18) or (A.Op = 20) then begin
    V := Expr(A.X, Term, Inputs, Env, Depth + 1);
    W := Expr(A.Y, Term, Inputs, Env, Depth + 1);
    if A.Op = 18 then begin
      if (V.Kind <> 2) or (W.Kind = 0) then Exit(Empty(1));
      R := Empty(0); R.Bindings.Count := 1;
      R.Bindings.Items[0].Key := V.N; R.Bindings.Items[0].Value := W; Exit(R);
    end;
    if (V.Kind <> 1) or (W.Kind <> 3) or (W.N < 0) or (W.N >= CellCount) then Exit(Empty(1));
    Cells[W.N] := V; Exit(Empty(0));
  end;
  if A.Op = 21 then Exit(RunNode(Terms[Term].Fields[A.N].N, Inputs, Env, Depth + 1));
  Left := Action(A.X, Term, Inputs, Env, Depth + 1);
  if A.Op = 22 then begin
    if Left.Status <> 0 then Exit(Left);
    R := Empty(0); R.OutValues := Left.OutValues; Merge(R.Bindings, Env, Left.Bindings, True); Exit(R);
  end;
  if A.Op = 26 then begin
    if Left.Status = 1 then Exit(Action(A.Y, Term, Inputs, Env, Depth + 1)) else Exit(Left);
  end;
  if A.Op = 27 then begin
    if Left.Status = 2 then Exit(Action(A.Y, Term, Left.OutValues, Env, Depth + 1)) else Exit(Left);
  end;
  if Left.Status <> 0 then Exit(Left);
  if A.Op = 23 then Right := Action(A.Y, Term, Left.OutValues, Env, Depth + 1)
  else if A.Op = 25 then Right := Action(A.Y, Term, Inputs, Left.Bindings, Depth + 1)
  else Right := Action(A.Y, Term, Inputs, Env, Depth + 1);
  if Right.Status <> 0 then Exit(Right);
  R := Empty(0);
  if A.Op = 25 then R.Bindings := Right.Bindings
  else if not Merge(R.Bindings, Left.Bindings, Right.Bindings, False) then Exit(Empty(1));
  if A.Op = 23 then R.OutValues := Right.OutValues
  else begin
    if Left.OutValues.Count + Right.OutValues.Count > 64 then Limit('transient result capacity');
    R.OutValues := Left.OutValues;
    for I := 0 to Right.OutValues.Count - 1 do begin
      R.OutValues.Items[R.OutValues.Count] := Right.OutValues.Items[I]; Inc(R.OutValues.Count);
    end;
  end;
  Result := R;
end;
function RunNode(Term: LongInt; const Inputs: TValues; const Env: TEnv; Depth: LongInt): TOutcome;
begin
  Tick(Depth); Result := Action(Ctors[Terms[Term].Ctor].Rule, Term, Inputs, Env, Depth + 1);
end;
function ReadSource(const Path: AnsiString; HasPath: Boolean): RawByteString;
var Stream: TStream; Buffer: array[0..4095] of AnsiChar; N, Want: LongInt;
begin
  Result := '';
  if HasPath then Stream := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone)
  else Stream := THandleStream.Create(TTextRec(Input).Handle);
  try
    repeat
      Want := 65537 - Length(Result); if Want > SizeOf(Buffer) then Want := SizeOf(Buffer);
      N := Stream.Read(Buffer, Want);
      if N = 0 then Break;
      SetLength(Result, Length(Result) + N);
      Move(Buffer[0], Result[Length(Result) - N + 1], N);
      if Length(Result) > 65536 then Limit('input exceeds 64 KiB');
    until False;
  finally Stream.Free end;
end;
function CLI: LongInt;
var
  TermText, Path, OutputPath, Flag, Arg, Text: AnsiString;
  HasTerm, HasPath, HasOutput: Boolean;
  I, Eq, Root: LongInt;
  R: TOutcome; Inputs: TValues; Env: TEnv; V: TValue; Stream: TStream;
begin
  HasTerm := False; HasPath := False; HasOutput := False; I := 1;
  while I <= ParamCount do begin
    Flag := ParamStr(I); Eq := Pos('=', Flag);
    if Eq <> 0 then Flag := Copy(Flag, 1, Eq - 1);
    if (Eq = 0) and ((Flag = '--help') or (Flag = '-h')) then begin
      WriteLn('generated interpreter [--term PROGRAM | --term-file PATH] [-o FILE]');
      WriteLn('Exit: completed=0, failed/escaped=1, errors=2.'); Exit(0);
    end;
    if (Eq = 0) and (Flag = '--version') then begin WriteLn('acsem @VERSION@'); Exit(0) end;
    if (Eq = 0) and (Flag = '--run') then begin Inc(I); Continue end;
    if (Flag <> '--term') and (Flag <> '--term-file') and (Flag <> '-o') and (Flag <> '--output') then Error('invalid option');
    if Eq <> 0 then Arg := Copy(ParamStr(I), Eq + 1, MaxInt)
    else begin Inc(I); if I > ParamCount then Error('missing option value'); Arg := ParamStr(I) end;
    if Flag = '--term' then begin TermText := Arg; HasTerm := True end
    else if Flag = '--term-file' then begin Path := Arg; HasPath := True end
    else begin OutputPath := Arg; HasOutput := True end;
    Inc(I);
  end;
  if HasTerm and HasPath then Error('use only one of --term and --term-file');
  if HasTerm then Source := TermText else Source := ReadSource(Path, HasPath);
  LoadModel; Lex; Root := ParseTerm(StartSort, 1);
  if Tokens[TokenPos].Kind <> 0 then Syntax('trailing term input');
  FillChar(Inputs, SizeOf(Inputs), 0); FillChar(Env, SizeOf(Env), 0);
  R := RunNode(Root, Inputs, Env, 1);
  case R.Status of 0: Text := 'completed'; 1: Text := 'failed'; 2: Text := 'escaped' end;
  for I := 0 to R.OutValues.Count - 1 do begin
    V := R.OutValues.Items[I]; Text := Text + ' ';
    if V.Kind = 2 then Text := Text + 'token:' + Symbols[V.N]
    else begin
      if V.Kind = 3 then Text := Text + 'cell:';
      Text := Text + IntToStr(V.N);
    end;
  end;
  Text := Text + #10;
  if HasOutput and (OutputPath <> '-') then Stream := TFileStream.Create(OutputPath, fmCreate)
  else Stream := THandleStream.Create(TTextRec(Output).Handle);
  try Stream.WriteBuffer(Text[1], Length(Text)) finally Stream.Free end;
  if R.Status = 0 then Result := 0 else Result := 1;
end;
var Status: LongInt;
begin
  try Status := CLI
  except
    on E: EAcsem do begin WriteLn(StdErr, E.Message); Status := 2 end;
    on E: Exception do begin WriteLn(StdErr, 'ACSEM_ERROR: ' + E.Message); Status := 2 end;
  end;
  Halt(Status);
end.
