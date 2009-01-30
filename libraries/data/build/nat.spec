#include bool.spec
#include pos.spec

sort Nat <"nat">;
% Auxiliary sort natpair, pair of natural numbers
     @NatPair <"natpair">;

cons @c0 <"c0"> : Nat;
     @cNat <"cnat"> : Pos <"arg"> -> Nat;
% Constructor for natpair
     @cPair <"cpair"> : Nat <"proj1"> # Nat <"proj2"> -> @NatPair;

map Pos2Nat <"pos2nat"> : Pos <"arg"> -> Nat;
    Nat2Pos <"nat2pos"> : Nat <"arg"> -> Pos;
    max <"max">:Pos <"left"> #Nat <"right">->Pos;
    max <"max">:Nat <"left"> #Pos <"right">->Pos;
    max <"max">:Nat <"left"> #Nat <"right">->Nat;
    min <"min">:Nat <"left"> #Nat <"right">->Nat;
    abs <"abs">:Nat <"number">->Nat;
    succ <"succ">:Nat <"number">->Pos;
    pred <"pred">:Pos <"number">->Nat;
    @dub <"dub">:Bool <"bit"> # Nat <"arg"> -> Nat;
    + <"plus">:Pos <"left"> #Nat <"right">->Pos;
    + <"plus">:Nat <"left"> #Pos <"right">->Pos;
    + <"plus">:Nat <"left"> #Nat <"right">->Nat;
    @gtesubt <"gtesubt">:Pos <"arg1"> # Pos <"arg2"> -> Nat;
    @gtesubt <"gtesubt">:Nat <"arg1"> # Nat <"arg2"> -> Nat;
    @gtesubtb <"gtesubtb">: Bool <"bit"> # Pos <"arg1"> # Pos <"arg2"> -> Nat;
    * <"times">:Nat <"left"> #Nat <"right">->Nat;
    div <"div">: Pos <"arg1"> # Pos <"arg2"> -> Nat;
    div <"div">: Nat <"arg1"> # Pos <"arg2"> -> Nat;
    mod <"mod">:Pos <"arg1"> # Pos <"arg2"> -> Nat;
    mod <"mod">:Nat <"arg1"> # Pos <"arg2"> -> Nat;
    exp <"exp">:Pos <"arg1"> # Nat <"arg2"> -> Pos;
    exp <"exp">:Nat <"arg1"> # Nat <"arg2"> -> Nat;
    @even <"even">:Nat <"arg"> -> Bool;

% functions for natpair
    @first <"first"> : @NatPair <"pair"> -> Nat;
    @last <"last"> : @NatPair <"pair"> -> Nat;
    @divmod <"divmod"> : Pos <"arg1"> # Pos <"arg2"> -> @NatPair;
    @gdivmod <"gdivmod"> : @NatPair <"arg1"> # Bool <"bit"> # Pos <"arg3"> -> @NatPair;
    @ggdivmod <"ggdivmod"> : Nat <"arg1"> # Nat <"arg2"> # Pos <"arg3"> -> @NatPair;


var b:Bool;
    c:Bool;
    p:Pos;
    q:Pos;
    n:Nat;
    m:Nat;
    u:Nat;
    v:Nat;
eqn ==(@c0, @cNat(p)) = false;
    ==(@cNat(p), @c0) = false;
    ==(@cNat(p), @cNat(q)) = ==(p,q);
    Pos2Nat = @cNat;
    Nat2Pos(@cNat(p)) = p;
    <=(@c0, n) = true;
    <=(@cNat(p), @c0) = false;
    <=(@cNat(p), @cNat(q)) = <=(p,q);
    <(n, @c0) = false;
    <(@c0, @cNat(p)) = true;
    <(@cNat(p), @cNat(q)) = <(p,q);
    max(p, @c0) = p;
    max(p, @cNat(q)) = if(<=(p,q),q,p);
    max(@c0,p) = p;
    max(@cNat(p),q) = if(<=(p,q),q,p);
    max(m,n) = if(<=(m,n),n,m);
    min(m,n) = if(<=(m,n),m,n);
    abs(n) = n;
    succ(@c0) = @c1;
    succ(@cNat(p)) = succ(p);
    succ(succ(n)) = @cDub(==(mod(n,@cDub(false,@c1)), @cNat(@c1)), succ(div(n,@cDub(false,@c1))));
    pred(@c1) = @c0;
    pred(@cDub(true,p)) = @cNat(@cDub(false,p));
    pred(@cDub(false,p)) = @dub(true,pred(p));
    @dub(false,@c0) = @c0;
    @dub(true,@c0) = @cNat(@c1);
    @dub(b,@cNat(p)) = @cNat(@cDub(b,p));
    +(p,@c0) = p;
    +(p,@cNat(q)) = @addc(false,p,q);
    +(@c0,p) = p;
    +(@cNat(p),q) = @addc(false,p,q);
    +(@c0,n) = n;
    +(n,@c0) = n;
    +(@cNat(p),@cNat(q)) = @cNat(@addc(false,p,q));
    @gtesubt(p,q) = @gtesubtb(false,p,q);
    @gtesubt(n,@c0) = n;
    @gtesubt(@cNat(p),@cNat(q)) = @gtesubtb(false, p,q);
    @gtesubtb(false,p,@c1) = pred(p);
    @gtesubtb(true,p,@c1) = pred(Nat2Pos(pred(p)));
    @gtesubtb(b,@cDub(c,p),@cDub(c,q)) = @dub(b, @gtesubtb(b,p,q));
    @gtesubtb(b,@cDub(false,p),@cDub(true,q)) = @dub(!(b),@gtesubtb(true,p,q));
    @gtesubtb(b,@cDub(true,p),@cDub(false,q)) = @dub(!(b),@gtesubtb(false,p,q));
    *(@c0,n) = @c0;
    *(n,@c0) = @c0;
    *(@cNat(p),@cNat(q)) = @cNat(*(p,q));
    exp(p,@c0) = @c1;
    exp(p,@cNat(@c1)) = p;
    exp(p,@cNat(@cDub(false,q))) = exp(@multir(false,@c1,p,p),@cNat(q));
    exp(p,@cNat(@cDub(true,q))) = @multir(false,@c1,p,exp(@multir(false,@c1,p,p),@cNat(q)));
    exp(n,@c0) = @cNat(@c1);
    exp(@c0, @cNat(p)) = @c0;
    exp(@cNat(p),n) = @cNat(exp(p,n));
    @even(@c0) = true;
    @even(@cNat(@c1)) = false;
    @even(@cNat(@cDub(b,p))) = !(b);
    div(p,@c1) = @cNat(p);
    div(@c1, @cDub(b,p)) = @c0;
    div(@cDub(b,p),@cDub(false,q)) = div(p,q);
    <=(p,q) -> div(@cDub(false,p), @cDub(true,q)) = @c0;
    <(q,p) -> div(@cDub(false,p), @cDub(true,q)) = @first(@gdivmod(@divmod(p, @cDub(true, q)), false, @cDub(true, q)));
    <=(p,q) -> div(@cDub(true,p), @cDub(true,q)) = if(==(p,q),@cNat(@c1),@c0);
    <(q,p) -> div(@cDub(true,p), @cDub(true,q)) = @first(@gdivmod(@divmod(p, @cDub(true,q)), true, @cDub(true,q)));
    div(@c0,p) = @c0;
    div(@cNat(p),q) = div(p,q);
    mod(p,@c1) = @c0;
    mod(@c1,@cDub(b,p)) = @cNat(@c1);
    mod(@cDub(b,p),@cDub(false,q)) = @dub(b,mod(p,q));
    <=(p,q) -> mod(@cDub(false,p),@cDub(true,q)) = @cNat(@cDub(false,p));
    <(q,p) -> mod(@cDub(false,p),@cDub(true,q)) = @last(@gdivmod(@divmod(p,@cDub(true,q)), false, @cDub(true, q)));
    <=(p,q) -> mod(@cDub(true,p),@cDub(true,q)) = if(==(p,q), @c0, @cNat(@cDub(true,p)));
    <(q,p) -> mod(@cDub(true,p),@cDub(true,q)) = @last(@gdivmod(@divmod(p,@cDub(true,q)), true, @cDub(true, q)));
    mod(@c0,p) = @c0;
    mod(@cNat(p),q) = mod(p,q);

% equations for natpair
    ==(@cPair(m,n), @cPair(u,v)) = &&(==(m,u),==(n,v));
    @first(@cPair(m,n)) = m;
    @last(@cPair(m,n)) = n;
    @divmod(@c1,@c1) = @cPair(@cNat(@c1),@c0);
    @divmod(@c1,@cDub(b,p)) = @cPair(@c0, @cNat(@c1));
    @divmod(@cDub(b,p),q) = @gdivmod(@divmod(p,q),b,q);
    @gdivmod(@cPair(m,n),b,p) = @ggdivmod(@dub(b,n),m,p);
    @ggdivmod(@c0,n,p) = @cPair(@dub(false,n),@c0);
    <(p,q) -> @ggdivmod(@cNat(p),n,q) = @cPair(@dub(false,n),@cNat(p));
    <=(q,p) -> @ggdivmod(@cNat(p),n,q) = @cPair(@dub(true,n),@gtesubtb(false,p,q));
