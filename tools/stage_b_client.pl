#!/usr/bin/env perl
# stage_b_client.pl — giza_server の段階 B を、窓を実際に出して確かめるテスト用のクライアント。
# 中身つきの PING で機能を問い合わせ、NEWWIN・3D_MESH・3D_VIEW を送り、そのあと決まった時間だけ
# サーバからのメッセージ(3D_POSE・3D_PICKV・3D_HOVERV・3D_INPUT)を受けて 1 行ずつ表示する。
#   stage_b_client.pl SOCK MESH.bin SECONDS
use strict; use warnings;
use IO::Socket::UNIX; use IO::Select; use Time::HiRes qw(time);
my ($sock, $meshfile, $secs) = @ARGV;
my $s = IO::Socket::UNIX->new(Type => SOCK_STREAM(), Peer => $sock) or die "connect $sock: $!\n";
my $seq = 0;
sub send_msg { my ($t, $p) = @_; $p //= ''; syswrite($s, pack('V C C v V V', 0x47495A41, 1, $t, 0, length $p, $seq++) . $p) }
sub read_n { my ($n) = @_; my $b = ''; while (length $b < $n) { my $r = sysread($s, $b, $n - length $b, length $b); die "eof\n" unless $r } $b }
sub recv_msg { my $h = read_n(16); my (undef, undef, $t, undef, $len) = unpack 'V C C v V V', $h; ($t, $len ? read_n($len) : '') }
$| = 1;
send_msg(0x01, pack 'V', 1);
my ($t, $p) = recv_msg();
printf "PONG type=0x%02X len=%d caps=%s\n", $t, length $p, length $p == 4 ? unpack('V', $p) : '-';
send_msg(0x12, pack 'V V', 400, 400); ($t) = recv_msg(); printf "NEWWIN -> 0x%02X\n", $t;
my $mesh = do { open my $f, '<:raw', $meshfile or die; local $/; <$f> };
send_msg(0x29, $mesh); ($t) = recv_msg(); printf "3D_MESH %d bytes -> 0x%02X\n", length $mesh, $t;
send_msg(0x2B, pack 'f<4 f< f<3 C', 0, 0, 0, 1, 2.0, 0, 0, 0, 1);   # Cz from above, zoom 2, perspective
print "3D_VIEW sent\n";
my $sel = IO::Select->new($s); my $end = time + $secs;
while ((my $left = $end - time) > 0) {
    next unless $sel->can_read($left);
    my ($ty, $pl) = recv_msg();
    if ($ty == 0x2D) { printf "POSE q=(%.4f %.4f %.4f %.4f) zoom=%.3f persp=%d\n", unpack 'f<4 f< x12 C', $pl }
    elsif ($ty == 0x2E) { printf "PICKV mesh=%d vertex=%d button=%d fx=%.3f fy=%.3f\n", unpack 'C V C f< f<', $pl }
    elsif ($ty == 0x2F) { my @v = unpack 'C V f< f<', $pl; $v[1] = -1 if $v[1] == 0xFFFFFFFF; printf "HOVERV mesh=%d vertex=%d\n", @v[0,1] }
    elsif ($ty == 0x25) { my @v = unpack 'f<3 C C', $pl; printf "3D_INPUT type=%d key=%s\n", $v[3], chr $v[4] }
    else { printf "msg 0x%02X len=%d\n", $ty, length $pl }
}
