# Detect Install
#
# The Interposer is not a machine-wide runtime - it is installed per game,
# next to the game executable, and its config is rendered from the per-game
# options. There is nothing to share between games, so always report "not
# installed" and let the install script run.

$Return = $False
