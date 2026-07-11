#!/usr/bin/env perl
# verify_wx_projection.pl
#   pdl_3d_wx.pl の「投影(Y反転)」と「クォータニオン・トラックボール回転」を
#   ヘッドレスで検証する。行列演算は plain Perl(PDL/Wx 不要)。ビューア本体の
#   数式と桁一致することは numpy/PDL で別途確認済み。
#
#   (1) 実ファイル pdl_3d_wx.pl の静的チェック:
#         - _project の $sy 2本が -Y 反転済み(persp/ortho 各1)
#         - トラックボール seed が恒等 [1,0,0,0]
#         - phi クランプ(1.57)が残っていない
#         - クォータニオン・ヘルパ一式が存在
#   (2) standard_1020.elc の実電極で:
#         - seed(恒等 q)で canonical: Fp上/O下/T3左/T4右/Cz手前/det>0
#         - 剛体性: ランダムドラッグ列でも det(R)>0 & q正規化維持
#         - 極なし: 連続縦ドラッグで Cz depth が符号反転を繰り返す
#
#   使い方: perl verify_wx_projection.pl [--src pdl_3d_wx.pl] [--elc standard_1020.elc]
#   依存: Test::More(コア)のみ。

use strict; use warnings;
use Test::More;
use Getopt::Long;

my $SRC='pdl_3d_wx.pl'; my $ELC='standard_1020.elc';
GetOptions('src=s'=>\$SRC,'elc=s'=>\$ELC);

# ---------- (1) 静的チェック ----------
SKIP: {
    unless (-r $SRC) { skip "$SRC not readable", 5; }
    open my $fh,'<',$SRC or die "open $SRC: $!";
    my $code=do{local $/; <$fh>}; close $fh;
    my @sy=$code=~/^\s*my\s+\$sy\b[^\n]*$/mg;
    my $syneg=grep { /-\s*\$rot3d->slice\(\s*',\(1\)'\s*\)/ } @sy;
    is(scalar @sy,2,"_project has exactly two \$sy branches");
    is($syneg,2,"both \$sy branches negate Y (screen-up)");
    ok($code=~/\{qrot\}\s*=\s*\[\s*1\s*,\s*0\s*,\s*0\s*,\s*0\s*\]/,"trackball seed is identity quaternion [1,0,0,0]");
    ok($code!~/1\.57/,"no leftover phi clamp (1.57)");
    my @need=qw(_q_axis_angle _q_mul _q_norm _q_to_rot _apply_drag_rotation);
    ok((grep { $code=~/sub \Q$_\E\b/ } @need)==@need,"quaternion helpers present (@need)");
}

# ---------- plain-Perl 数学(ビューアと同一の式) ----------
sub q_axis_angle { my ($ax,$ay,$az,$a)=@_; my $s=sin($a*0.5); (cos($a*0.5),$ax*$s,$ay*$s,$az*$s) }
sub q_mul { my ($aw,$ax,$ay,$az,$bw,$bx,$by,$bz)=@_;
    ($aw*$bw-$ax*$bx-$ay*$by-$az*$bz, $aw*$bx+$ax*$bw+$ay*$bz-$az*$by,
     $aw*$by-$ax*$bz+$ay*$bw+$az*$bx, $aw*$bz+$ax*$by-$ay*$bx+$az*$bw) }
sub q_norm { my ($w,$x,$y,$z)=@_; my $l=sqrt($w*$w+$x*$x+$y*$y+$z*$z); $l<1e-12?(1,0,0,0):($w/$l,$x/$l,$y/$l,$z/$l) }
sub q_to_rot { my ($w,$x,$y,$z)=@_;
    my ($x2,$y2,$z2)=($x*$x,$y*$y,$z*$z); my ($xy,$xz,$yz)=($x*$y,$x*$z,$y*$z); my ($wx,$wy,$wz)=($w*$x,$w*$y,$w*$z);
    [ [1-2*($y2+$z2),2*($xy-$wz),2*($xz+$wy)],
      [2*($xy+$wz),1-2*($x2+$z2),2*($yz-$wx)],
      [2*($xz-$wy),2*($yz+$wx),1-2*($x2+$y2)] ] }
sub det3 { my ($m)=@_;
    $m->[0][0]*($m->[1][1]*$m->[2][2]-$m->[1][2]*$m->[2][1])
   -$m->[0][1]*($m->[1][0]*$m->[2][2]-$m->[1][2]*$m->[2][0])
   +$m->[0][2]*($m->[1][0]*$m->[2][1]-$m->[1][1]*$m->[2][0]) }
sub proj { my ($R,$p)=@_;
    my @r=map { $R->[$_][0]*$p->[0]+$R->[$_][1]*$p->[1]+$R->[$_][2]*$p->[2] } 0..2;
    ($r[0],-$r[1],$r[2]) }
sub apply_drag { my ($q,$dx,$dy)=@_;
    my @ry=q_axis_angle(0,1,0,+1*$dx*0.01);   # DRAG_DX_SIGN=+1
    my @rx=q_axis_angle(1,0,0,+1*$dy*0.01);   # DRAG_DY_SIGN=+1
    [ q_norm(q_mul(@rx,q_mul(@ry,@$q))) ] }

sub parse_elc { my ($path)=@_;
    open my $fh,'<',$path or die "open $path: $!";
    my (@pos,@lab); my $mode='';
    while (<$fh>) { s/^\s+//; s/\s+$//; next if $_ eq ''||/^#/;
        if (/^Positions\b/i){$mode='pos';next}
        if (/^Labels\b/i){$mode='lab';next}
        next if /^(NumberPositions|UnitPosition|ReferenceLabel)/i;
        if ($mode eq 'pos'){ my @c=split /\s+/; push @pos,[@c[0,1,2]] if @c>=3 }
        elsif ($mode eq 'lab'){ push @lab,$_ } }
    return (\@pos,\@lab) }

# ---------- (2) 実電極での検証 ----------
SKIP: {
    unless (-r $ELC) { skip "$ELC not readable", 9; }
    my ($pos,$lab)=parse_elc($ELC);
    my %i; $i{$lab->[$_]}=$_ for 0..$#$lab;
    is(scalar @$pos,scalar @$lab,"elc: positions == labels");

    my $R0=q_to_rot(1,0,0,0);
    my (%sx,%sy,%dz);
    for my $l (qw(Fpz Oz Cz T7 T8 T3 T4)) {
        my ($x,$y,$z)=proj($R0,$pos->[$i{$l}]); $sx{$l}=$x; $sy{$l}=$y; $dz{$l}=$z; }
    my $maxdepth=(sort { $b<=>$a } map { (proj($R0,$_))[2] } @$pos)[0];
    ok($sy{Fpz}<$sy{Oz},"[seed] Fp above O");
    ok($sy{Fpz}<$sy{Cz},"[seed] Fp above Cz");
    ok($sx{T7}<$sx{T8},"[seed] T7(left) < T8(right)");
    ok($sx{T3}<$sx{T4},"[seed] T3(left) < T4(right)");
    ok(abs($dz{Cz}-$maxdepth)<1e-9,"[seed] Cz nearest viewer");
    ok(det3($R0)>0,"[seed] non-mirror (det>0)");

    srand(42);
    my $q=[1,0,0,0]; my $detbad=0; my $normbad=0; my $N=2000;
    for (1..$N) { my $dx=rand()*80-40; my $dy=rand()*80-40;
        $q=apply_drag($q,$dx,$dy); my $R=q_to_rot(@$q);
        $detbad++ if det3($R)<=0;
        my ($w,$x,$y,$z)=@$q; $normbad++ if abs(sqrt($w*$w+$x*$x+$y*$y+$z*$z)-1)>1e-9; }
    is($detbad,0,"rigidity: det(R)>0 over $N random drags (never mirrored)");
    is($normbad,0,"rigidity: quaternion stays normalized over $N drags");

    my $q2=[1,0,0,0]; my $ci=$i{Cz}; my $prev; my $flips=0;
    for (1..160) { $q2=apply_drag($q2,0,15); my $R=q_to_rot(@$q2);
        my $d=(proj($R,$pos->[$ci]))[2];
        $flips++ if defined $prev && ($d<=>0)!=($prev<=>0) && $prev!=0; $prev=$d; }
    cmp_ok($flips,'>=',4,"pole-free: Cz passes through poles repeatedly ($flips sign flips)");
}
done_testing();
