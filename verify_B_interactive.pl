#!/usr/bin/env perl
# =============================================================
# verify_B_interactive.pl  —  manual check for server12 item [B]
#
#   [B] Closing an interactive window (red button) must make the
#       giza_server shutdown() the client socket, so show_interactive's
#       run loop gets EOF and RETURNS — WITHOUT needing Cmd-Q.
#
# Run from the PDL-Graphics-Cairo/ root, against the NEWLY BUILT giza_server:
#
#   # make sure the patched giza_server is the one used:
#   GIZA_SERVER=../giza-server/giza_server \
#     perl -I./lib examples/verify_B_interactive.pl
#
# (omit GIZA_SERVER if the patched giza_server is already on PATH or the
#  sibling ../giza-server checkout is auto-discovered by Driver::GS.)
#
# WHAT TO DO:
#   1. A window titled "verify_B [B] close-test" opens, showing a sine wave
#      with two sliders (k = horizontal, A = vertical).
#   2. Optionally drag the sliders to confirm the live reverse channel.
#   3. Close the window with the RED BUTTON (or, if tabbed, the tab's x).
#      Do NOT press Cmd-Q.
#
# EXPECTED (fix working):
#   The moment you close the window, this script prints
#       "[B] PASS: show_interactive returned after window close (...s)"
#   and exits on its own.
#
# OLD BEHAVIOUR (no fix):
#   Closing the window does nothing; the script hangs. Only Cmd-Q (quitting
#   giza_server) would end it. If you have to use Cmd-Q, [B] is NOT fixed.
# =============================================================
use strict; use warnings;
use PDL;
use PDL::Graphics::Cairo qw(figure);
use PDL::Graphics::Cairo::Driver::GS;

$| = 1;
my $PI = 3.14159265358979;

my $gs = PDL::Graphics::Cairo::Driver::GS->new(
    width => 640, height => 480,
    title => 'verify_B [B] close-test',
);

print "[B] window opening. Close it with the RED BUTTON (NOT Cmd-Q).\n";
print "    If this script exits on close -> PASS. If it hangs -> FAIL.\n\n";

my $t0 = time;

$gs->show_interactive(
    init   => { 0 => 1.0, 1 => 1.0 },
    render => sub {
        my ($s) = @_;
        my $k = $s->{0} // 1.0;
        my $A = $s->{1} // 1.0;
        my $x = sequence(500) / 499 * (2 * $PI);
        my $y = $A * sin($k * $x);
        my $fig = figure(width => 640, height => 480);
        my ($ax) = $fig->subplots(1, 1);
        $ax->line($x, $y);
        $ax->ylim(-2.2, 2.2);
        $fig->tight_layout;
        return $fig;
    },
);

# Reached ONLY if show_interactive returned, i.e. the run loop saw EOF.
printf "\n[B] PASS: show_interactive returned after window close (%ds elapsed).\n",
       time - $t0;
print  "    The run loop received EOF from the server on close — no Cmd-Q needed.\n";
