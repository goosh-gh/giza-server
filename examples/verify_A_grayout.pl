#!/usr/bin/env perl
# =============================================================
# verify_A_grayout.pl  —  manual check for server12 item [A]
#
#   [A] On a CLIENT-ABSENT window (a plain show() whose script has exited),
#       File > Save as PDF / SVG must be GRAYED OUT (no client left to
#       re-render), while Save as PNG stays ENABLED (served from last_png).
#       On a LIVE show_interactive window all three are enabled.
#
# Run from the PDL-Graphics-Cairo/ root, against the NEWLY BUILT giza_server:
#
#   GIZA_SERVER=../giza-server/giza_server \
#     perl -I./lib examples/verify_A_grayout.pl
#
# WHAT HAPPENS:
#   This does a PLAIN show() and then EXITS. The window persists (that is the
#   pgxwin_server-style persistence), but its client is now gone.
#
# WHAT TO CHECK (after the shell prompt returns):
#   Click the persisted window "verify_A [A] client-absent", open the File menu:
#       Save as PDF …    -> GRAYED OUT   (expected)
#       Save as SVG …    -> GRAYED OUT   (expected)
#       Save as PNG …    -> ENABLED      (expected; saves from last_png)
#
#   Contrast: while verify_B_interactive.pl is still RUNNING (live client),
#   all three items are ENABLED. That contrast is the whole point of [A].
# =============================================================
use strict; use warnings;
use PDL;
use PDL::Graphics::Cairo qw(figure);
use PDL::Graphics::Cairo::Driver::GS;

$| = 1;
my $PI = 3.14159265358979;

my $x = sequence(500) / 499 * (2 * $PI);
my $y = sin($x);

my $fig = figure(width => 560, height => 400);
my ($ax) = $fig->subplots(1, 1);
$ax->line($x, $y);
$ax->ylim(-1.2, 1.2);
$fig->tight_layout;

# Use Driver::GS explicitly (NOT $fig->show()).  $fig->show() goes through
# _default_backend(), and an OLD installed P:G:Cairo (pre driver-unification)
# may still route that to the retired OSX viewer (pdlcairo_viewer) instead of
# giza_server — which would not exercise the giza_server File menu at all.
# Driver::GS->show() sends the PNG then closes the socket WITHOUT a CLOSE
# message, so the window persists as a CLIENT-ABSENT giza_server window.
my $gs = PDL::Graphics::Cairo::Driver::GS->new(
    width => 560, height => 400,
    title => 'verify_A [A] client-absent',
);
$gs->show($fig);

print "[A] plain show() done; this script is exiting now.\n";
print "    The window stays open as a CLIENT-ABSENT window.\n";
print "    Open its File menu:\n";
print "      Save as PDF / SVG  -> should be GRAYED OUT\n";
print "      Save as PNG        -> should be ENABLED\n";
