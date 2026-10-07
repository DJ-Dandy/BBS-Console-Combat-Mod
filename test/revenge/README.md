Offline check of the Revenge Value script against the game's own boss AI scripts.

    BBS_LUA51=<patched lua 5.1>  RV_LUB=<folder with the game's .lub files>
    $BBS_LUA51 test/revenge/extra.lua                      edge cases of the four bosses added here
    $BBS_LUA51 test/revenge/run.lua <script> <entity> 300  hit strings: at which hit the boss breaks out
    RV_FACTORY=<original Factory.lub> ... run.lua ...      the same without the mod

`emu.lua` is a stand-in for the engine: every engine function exists and returns a neutral value; the few that the
damage callbacks depend on return fixed numbers.  The scripts themselves (common scripts, state machine, bosses)
are the game's.
