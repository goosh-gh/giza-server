#!/usr/bin/env perl
# pdl_3d_wx.pl  --  Phase 1: PDL + wxWidgets による3Dビューア
#
# 機能:
#   - 点群 / ワイヤーフレームグリッド / XYZ軸の3D描画
#   - マウスドラッグで回転、ホイールでズーム
#   - Z深度ソートによるペインターズアルゴリズム
#   - 透視投影 (正投影との切り替えスイッチ付き)
#   - ダブルバッファリングでちらつきなし
#
# 使い方:
#   perl pdl_3d_wx.pl [--mode scatter|grid|electrode]
#
# 依存: Wx (wxPerl), PDL

use FindBin;
use lib "$FindBin::Bin/wx/lib", "$FindBin::Bin/wx/arch";

use strict;
use warnings;
use Wx qw(:everything);
use PDL;
use PDL::Basic qw(sequence);
use Getopt::Long;


# ============================================================
# シーンデータ生成ヘルパー
# ============================================================
package Scene;

sub scatter {
    # ランダム点群  [N, 3]
    my $n = 200;
    my $xyz = PDL::grandom($n, 3) * 80;
    my $col = PDL::random($n, 3);  # RGB per-point  [N, 3]
    return { type => 'scatter', points => $xyz, colors => $col };
}

sub grid {
    # meshgrid サーフェス z = sin(r)/r
    my $n = 20;
    my $v = PDL::Basic::sequence($n) * (8 * 3.14159 / ($n - 1)) - 4 * 3.14159;
    my $x = $v->dummy(1, $n)->flat;          # [N*N]
    my $y = $v->dummy(0, $n)->flat;
    my $r = ($x**2 + $y**2)**0.5 + 1e-6;
    my $z = PDL::sin($r) / $r * 60;
    my $x_s = $x * 8;
    my $y_s = $y * 8;

    # 格子線の端点リスト: [M, 6] (x0,y0,z0,x1,y1,z1)
    my @lines;
    for my $i (0 .. $n - 1) {
        for my $j (0 .. $n - 2) {
            # 行方向
            my $idx_a = $i * $n + $j;
            my $idx_b = $i * $n + $j + 1;
            push @lines, [
                $x_s->at($idx_a), $y_s->at($idx_a), $z->at($idx_a),
                $x_s->at($idx_b), $y_s->at($idx_b), $z->at($idx_b),
                0.2, 0.6, 0.9,   # RGB
            ];
            # 列方向
            my $idx_c = $j * $n + $i;
            my $idx_d = ($j + 1) * $n + $i;
            push @lines, [
                $x_s->at($idx_c), $y_s->at($idx_c), $z->at($idx_c),
                $x_s->at($idx_d), $y_s->at($idx_d), $z->at($idx_d),
                0.9, 0.4, 0.2,
            ];
        }
    }
    my $lines_pdl = PDL->pdl(\@lines)->transpose;  # [M, 9] dim0=M lines, dim1=9 elems

    # 点（格子点）
    my $pts = PDL::cat($x_s, $y_s, $z);  # [N*N, 3] dim0=points, dim1=XYZ
    my $zn  = ($z - $z->min) / ($z->max - $z->min + 1e-9);
    # viridis風カラーマップ
    my $r_ch = $zn * 0.8 + 0.1;
    my $g_ch = PDL::sin($zn * 3.14159) * 0.8;
    my $b_ch = (1 - $zn) * 0.9;
    my $col  = PDL::cat($r_ch, $g_ch, $b_ch);  # [N*N, 3]

    return { type => 'grid', points => $pts, colors => $col, lines => $lines_pdl };
}

sub electrode {
    # 10-20 電極システム近似（球面座標）
    # 各行: [label, bearing_deg, elevation_deg]
    #   bearing_deg   : 水平方位角。0=前方(Fp方向), 90=右(T方向), 180=後方(O方向), -90=左
    #   elevation_deg : 頭頂からの傾き。0=頭頂(Cz), 90=側頭/前後の縁(耳の高さ)
    my @elec = (
        # label   bearing  elevation
        ['Fp1',    -18,   80], ['Fp2',     18,   80],
        ['F7',     -54,   80], ['F3',     -27,   50], ['Fz',   0,   40], ['F4',  27,   50], ['F8',   54,   80],
        ['T3',     -90,   80], ['C3',     -45,   40], ['Cz',    0,    0], ['C4',  45,   40], ['T4',   90,   80],
        ['T5',    -126,   80], ['P3',    -153,   50], ['Pz',  180,   40], ['P4', 153,   50], ['T6',  126,   80],
        ['O1',    -162,   80], ['Oz',    180,   80],  ['O2',  162,   80],
    );
    my $r = 100;
    my (@x, @y, @z, @labels);
    for my $e (@elec) {
        my ($lbl, $bearing_deg, $elev_deg) = @$e;
        my $bearing = $bearing_deg * 3.14159 / 180;
        my $elev    = $elev_deg    * 3.14159 / 180;
        push @x, $r * sin($elev) * sin($bearing);
        push @y, $r * sin($elev) * cos($bearing);
        push @z, $r * cos($elev);
        push @labels, $lbl;
    }
    my $pts = PDL->pdl(\@x, \@y, \@z);  # [N, 3] dim0=points, dim1=XYZ
    # Z値でカラー（赤=高、青=低）
    my $zn  = ($pts->slice(',(2)') + $r) / (2 * $r);
    my $col = PDL::cat($zn, 1 - $zn, PDL::zeros($zn));
    return { type => 'electrode', points => $pts, colors => $col, labels => \@labels };
}

# ============================================================
# 3D Canvas ウィジェット
# ============================================================
package My3DCanvas;
use strict;
use warnings;
use Wx qw(:everything);
use base qw(Wx::Window);
use PDL;

use constant {
    AXIS_LEN  => 120,
    FOV       => 600,   # 透視投影焦点距離(px)
    WIN_CX    => 300,
    WIN_CY    => 300,

    # トラックボール回転(クォータニオン, GS3D と同方式)
    DRAG_SENSITIVITY => 0.01,   # ドラッグ1pxあたりの回転角(rad)。旧theta/phi感度と同じ
    DRAG_DX_SIGN     => +1,      # 横ドラッグ→world-Y。逆に感じたら -1
    DRAG_DY_SIGN     => +1,      # 縦ドラッグ→world-X。逆に感じたら -1
};

sub new {
    my ($class, $parent, $scene) = @_;
    my $self = $class->SUPER::new(
        $parent, -1, wxDefaultPosition, [600, 600],
        wxFULL_REPAINT_ON_RESIZE
    );

    $self->{scene}       = $scene;
    # 姿勢はクォータニオン qrot に累積し、rot(3x3)をそこから導く。
    # seed=恒等(1,0,0,0): wx は Y反転を _project(sy=-Y)側に置くので、
    # 恒等回転がそのまま標準トポマップ(Fp上/T3左/T4右/Cz手前)になる
    # (GS3D は screen-x 反転のため seed が (0,0,0,1) だが、wx は恒等でよい)。
    $self->{qrot}        = [ 1, 0, 0, 0 ];
    $self->{rot}         = _q_to_rot(@{$self->{qrot}});
    $self->{zoom}        = 1.0;
    $self->{perspective} = 1;   # 1=透視, 0=正投影
    $self->{last_x}      = 0;
    $self->{last_y}      = 0;

    # 軸定義 [4, 3] : 原点 + X先端 + Y先端 + Z先端
    my $al = AXIS_LEN;
    $self->{axes_xyz} = PDL->pdl([
        [0, $al, 0,   0  ],  # X
        [0, 0,   $al, 0  ],  # Y
        [0, 0,   0,   $al],  # Z
    ]);

    Wx::Event::EVT_PAINT($self,      \&on_paint);
    Wx::Event::EVT_LEFT_DOWN($self,  \&on_mouse_down);
    Wx::Event::EVT_MOTION($self,     \&on_mouse_move);
    Wx::Event::EVT_MOUSEWHEEL($self, \&on_wheel);
    Wx::Event::EVT_KEY_DOWN($self,   \&on_key);
    Wx::Event::EVT_SIZE($self,       sub { $_[0]->Refresh });

    return $self;
}

# ----------------------------------------------------------
# クォータニオン・ヘルパ (GS3D / giza-3d-quat.c と同一。純関数)
# クォータニオンは (w,x,y,z)。
# ----------------------------------------------------------
sub _q_axis_angle {            # axis は単位ベクトル
    my ($ax, $ay, $az, $ang) = @_;
    my $s = sin($ang * 0.5);
    return (cos($ang * 0.5), $ax*$s, $ay*$s, $az*$s);
}
sub _q_mul {                   # Hamilton積 a*b (bを先に、次にaを適用)
    my ($aw,$ax,$ay,$az, $bw,$bx,$by,$bz) = @_;
    return (
        $aw*$bw - $ax*$bx - $ay*$by - $az*$bz,
        $aw*$bx + $ax*$bw + $ay*$bz - $az*$by,
        $aw*$by - $ax*$bz + $ay*$bw + $az*$bx,
        $aw*$bz + $ax*$by - $ay*$bx + $az*$bw,
    );
}
sub _q_norm {
    my ($w,$x,$y,$z) = @_;
    my $len = sqrt($w*$w + $x*$x + $y*$y + $z*$z);
    return (1,0,0,0) if $len < 1e-12;
    return ($w/$len, $x/$len, $y/$len, $z/$len);
}
# クォータニオン -> 3x3回転行列(行優先)。$R x $pts[N,3] が転置なしで out=R.v になる。
sub _q_to_rot {
    my ($w,$x,$y,$z) = @_;
    my ($x2,$y2,$z2) = ($x*$x,$y*$y,$z*$z);
    my ($xy,$xz,$yz) = ($x*$y,$x*$z,$y*$z);
    my ($wx,$wy,$wz) = ($w*$x,$w*$y,$w*$z);
    return PDL->pdl(
        [1-2*($y2+$z2),   2*($xy-$wz),     2*($xz+$wy)   ],
        [2*($xy+$wz),     1-2*($x2+$z2),   2*($yz-$wx)   ],
        [2*($xz-$wy),     2*($yz+$wx),     1-2*($x2+$y2) ],
    );
}

# ----------------------------------------------------------
# ドラッグ差分(dx,dy)から world軸まわりの小回転を作り、現在の姿勢に
# 左から合成して正規化する(GS3D _apply_drag_rotation と同一)。
#   横ドラッグ(dx) -> world-Y軸まわり
#   縦ドラッグ(dy)  -> world-X軸まわり
# 極が無いので真上/真下を越えて連続的に回り続けられる(クランプ不要)。
# ----------------------------------------------------------
sub _apply_drag_rotation {
    my ($self, $dx, $dy) = @_;
    my $q  = $self->{qrot};
    my @ry = _q_axis_angle(0, 1, 0, DRAG_DX_SIGN * $dx * DRAG_SENSITIVITY);
    my @rx = _q_axis_angle(1, 0, 0, DRAG_DY_SIGN * $dy * DRAG_SENSITIVITY);
    my @r  = _q_mul(@rx, _q_mul(@ry, @$q));
    $self->{qrot} = [ _q_norm(@r) ];
    $self->{rot}  = _q_to_rot(@{$self->{qrot}});
}

# ----------------------------------------------------------
# 回転行列を返す [3,3]。状態(qrot由来のrot)を返すだけ。
# ----------------------------------------------------------
sub _rotation {
    my ($self) = @_;
    return $self->{rot};
}

# ----------------------------------------------------------
# 3D→2Dスクリーン投影 (PDL ndarray [3, N] → sx[N], sy[N])
# cx,cy: ウィンドウ中心、scale: 倍率
# ----------------------------------------------------------
sub _project {
    my ($self, $rot3d, $cx, $cy) = @_;
    # $rot3d: [N, 3]  dim0=N points, dim1=XYZ
    my $z    = $rot3d->slice(',(2)');   # [N]
    my $zoom = $self->{zoom};

    if ($self->{perspective}) {
        my $fov = FOV * $zoom;
        my $w   = $fov / ($fov - $z + 200);   # 奥ほどw<1
        my $sx  =  $rot3d->slice(',(0)') * $w * $zoom + $cx;
        my $sy  = -$rot3d->slice(',(1)') * $w * $zoom + $cy;   # Y反転: モデル+Y(前)→画面上
        return ($sx, $sy, $z);
    } else {
        my $sx =  $rot3d->slice(',(0)') * $zoom + $cx;
        my $sy = -$rot3d->slice(',(1)') * $zoom + $cy;         # Y反転
        return ($sx, $sy, $z);
    }
}

# ----------------------------------------------------------
# on_paint
# ----------------------------------------------------------
sub on_paint {
    my ($self, $event) = @_;
    my $dc = Wx::AutoBufferedPaintDC->new($self);
    $dc->SetBackground(Wx::Brush->new(Wx::Colour->new(20, 20, 30), wxSOLID));
    $dc->Clear();

    my ($w, $h) = $self->GetSizeWH;
    my $cx = $w / 2;
    my $cy = $h / 2;

    my $rot    = $self->_rotation();
    my $scene  = $self->{scene};

    # ---- XYZ軸 ----
    my $rot_ax   = $rot x $self->{axes_xyz};  # [4, 3] dim0=4points, dim1=XYZ
    my ($ax, $ay) = $self->_project($rot_ax, $cx, $cy);
    my ($ox, $oy) = ($ax->at(0), $ay->at(0));

    my @axis_colors = (
        [255, 80,  80 ],  # X 赤
        [80,  255, 80 ],  # Y 緑
        [80,  160, 255],  # Z 青
    );
    my @axis_labels = ('X', 'Y', 'Z');
    for my $i (0..2) {
        my $c = $axis_colors[$i];
        $dc->SetPen(Wx::Pen->new(Wx::Colour->new(@$c), 2, wxSOLID));
        $dc->DrawLine($ox, $oy, $ax->at($i+1), $ay->at($i+1));
        $dc->SetTextForeground(Wx::Colour->new(@$c));
        $dc->DrawText($axis_labels[$i], $ax->at($i+1)+6, $ay->at($i+1)-8);
    }

    # ---- ワイヤーフレーム線（グリッド用）----
    if (exists $scene->{lines}) {
        my $lines = $scene->{lines};   # [M, 9]: x0y0z0 x1y1z1 rgb
        my $n_lines = $lines->dim(0);

        # 始点・終点を回転
        my $p0 = $lines->slice(':, 0:2');  # [M, 3] dim0=M lines, dim1=XYZ
        my $p1 = $lines->slice(':, 3:5');  # [M, 3]
        my $r0 = $rot x $p0;
        my $r1 = $rot x $p1;
        my ($sx0, $sy0) = $self->_project($r0, $cx, $cy);
        my ($sx1, $sy1) = $self->_project($r1, $cx, $cy);

        # Z深度（線分中点）でソート
        my $zmid = ($r0->slice(',(2)') + $r1->slice(',(2)')) / 2;
        my $order = $zmid->qsorti;   # 奥から手前

        my $rgb = $lines->slice(':, 6:8');  # [M, 3]

        for my $idx ($order->list) {
            my ($r, $g, $b) = map { int($rgb->at($idx, $_) * 255) } 0..2;
            $dc->SetPen(Wx::Pen->new(Wx::Colour->new($r, $g, $b), 1, wxSOLID));
            $dc->DrawLine(
                $sx0->at($idx), $sy0->at($idx),
                $sx1->at($idx), $sy1->at($idx),
            );
        }
    }

    # ---- 点群（Z深度ソート）----
    if (exists $scene->{points}) {
        my $pts    = $scene->{points};   # [N, 3]
        my $colors = $scene->{colors};   # [N, 3]
        my $rot_pts = $rot x $pts;   # [N, 3] dim0=N points, dim1=XYZ
        my ($sx, $sy, $depth) = $self->_project($rot_pts, $cx, $cy);
        my $order = $depth->qsorti;             # 奥から手前

        my $n = $pts->dim(0);
        for my $idx ($order->list) {
            my ($r, $g, $b) = map { int($colors->at($idx, $_) * 255) } 0..2;
            my $sz = ($self->{zoom} * 4) > 1 ? int($self->{zoom} * 4) : 1;
            $dc->SetPen(Wx::Pen->new(Wx::Colour->new($r, $g, $b), 1, wxSOLID));
            $dc->SetBrush(Wx::Brush->new(Wx::Colour->new($r, $g, $b), wxSOLID));
            $dc->DrawCircle($sx->at($idx), $sy->at($idx), $sz);
        }

        # ---- 電極ラベル ----
        if (exists $scene->{labels}) {
            $dc->SetTextForeground(Wx::Colour->new(255, 255, 200));
            my $labels = $scene->{labels};
            for my $i (0 .. $#$labels) {
                $dc->DrawText($labels->[$i],
                    $sx->at($i) + 6,
                    $sy->at($i) - 6,
                );
            }
        }
    }

    # ---- ステータス表示 ----
    my $proj = $self->{perspective} ? 'Perspective' : 'Orthographic';
    $dc->SetTextForeground(Wx::Colour->new(180, 180, 180));
    $dc->DrawText(
        sprintf("zoom=%.2f [%s]  drag=rotate (free)  P:toggle proj  R:reset",
            $self->{zoom}, $proj),
        8, $h - 22
    );
}

# ----------------------------------------------------------
# イベントハンドラ
# ----------------------------------------------------------
sub on_mouse_down {
    my ($self, $event) = @_;
    $self->{last_x} = $event->GetX;
    $self->{last_y} = $event->GetY;
    $self->SetFocus;
}

sub on_mouse_move {
    my ($self, $event) = @_;
    return unless $event->Dragging && $event->LeftIsDown;
    my $dx = $event->GetX - $self->{last_x};
    my $dy = $event->GetY - $self->{last_y};
    $self->_apply_drag_rotation($dx, $dy);   # 極なし・クランプ不要で連続回転
    $self->{last_x} = $event->GetX;
    $self->{last_y} = $event->GetY;
    $self->Refresh;
}

sub on_wheel {
    my ($self, $event) = @_;
    my $delta = $event->GetWheelRotation;
    if ($delta > 0) {
        $self->{zoom} *= 1.1;
    } else {
        $self->{zoom} /= 1.1;
    }
    $self->{zoom} = 0.1 if $self->{zoom} < 0.1;
    $self->{zoom} = 10  if $self->{zoom} > 10;
    $self->Refresh;
}

sub on_key {
    my ($self, $event) = @_;
    my $key = $event->GetKeyCode;
    if ($key == ord('P') || $key == ord('p')) {
        $self->{perspective} = !$self->{perspective};
        $self->Refresh;
    } elsif ($key == ord('R') || $key == ord('r')) {
        # リセット: 標準トポマップ(真上から見下ろし)= 恒等クォータニオン
        $self->{qrot} = [ 1, 0, 0, 0 ];
        $self->{rot}  = _q_to_rot(@{$self->{qrot}});
        $self->{zoom} = 1.0;
        $self->Refresh;
    }
    $event->Skip;
}

# ============================================================
# メインフレーム（シーン選択タブ付き）
# ============================================================
package MainFrame;
use strict;
use warnings;
use Wx qw(:everything);
use base qw(Wx::Frame);

sub new {
    my ($class, $mode) = @_;
    my $self = $class->SUPER::new(
        undef, -1,
        'PDL 3D Viewer  [drag=rotate / wheel=zoom / P=proj / R=reset]',
        wxDefaultPosition, [640, 680],
    );

    my $panel  = Wx::Panel->new($self, -1);
    my $vbox   = Wx::BoxSizer->new(wxVERTICAL);

    # モード切替ボタンバー
    my $hbox = Wx::BoxSizer->new(wxHORIZONTAL);
    for my $m (qw(scatter grid electrode)) {
        my $btn = Wx::Button->new($panel, -1, ucfirst($m));
        $hbox->Add($btn, 1, wxEXPAND | wxALL, 2);
        Wx::Event::EVT_BUTTON($self, $btn, sub {
            $self->_switch_scene($m);
        });
    }
    $vbox->Add($hbox, 0, wxEXPAND | wxALL, 4);

    # 初期シーン
    my $scene   = $self->_make_scene($mode // 'scatter');
    my $canvas  = My3DCanvas->new($panel, $scene);
    $self->{canvas} = $canvas;
    $vbox->Add($canvas, 1, wxEXPAND | wxALL, 0);

    $panel->SetSizer($vbox);
    return $self;
}

sub _make_scene {
    my ($self, $mode) = @_;
    return Scene::scatter()   if $mode eq 'scatter';
    return Scene::grid()      if $mode eq 'grid';
    return Scene::electrode() if $mode eq 'electrode';
    return Scene::scatter();
}

sub _switch_scene {
    my ($self, $mode) = @_;
    $self->{canvas}{scene} = $self->_make_scene($mode);
    $self->{canvas}->Refresh;
}

# ============================================================
# エントリポイント
# ============================================================
package main;
use strict;
use warnings;
use Getopt::Long;

my $mode = 'scatter';
GetOptions('mode=s' => \$mode);

my $app   = Wx::SimpleApp->new;
my $frame = MainFrame->new($mode);
$frame->Show(1);
$app->MainLoop;
