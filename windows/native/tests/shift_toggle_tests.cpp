#include "../shift_toggle.hpp"
#include <cassert>
#include <iostream>
using boshiamy::ShiftToggleTracker;
int main() {
 ShiftToggleTracker t; t.set_enabled(true);
 auto tap=[&](unsigned down,unsigned up){t.key_down(down,false,false,false); bool claim=t.would_toggle(up,false,false,false); bool result=t.key_up(up,false,false,false); assert(claim==result);return result;};
 assert(tap(0x10,0x10)); assert(tap(0xa0,0xa0)); assert(tap(0xa1,0xa1));
 assert(tap(0xa0,0x10)); assert(tap(0xa1,0x10)); assert(tap(0x10,0xa0)); assert(tap(0x10,0xa1));
 t.key_down(0xa0,false,false,false); assert(!t.key_up(0xa1,false,false,false)); t.reset();
 t.key_down(0xa0,false,false,false); t.key_down('A',false,false,false); assert(!t.key_up(0xa0,false,false,false));
 t.key_down(0xa0,false,false,false); t.key_down(0xa1,false,false,false,false,true); assert(!t.key_up(0x10,false,false,false)); t.reset();
 t.key_down(0xa0,false,false,false); assert(!t.key_up(0xa0,true,false,false));
 t.key_down(0xa0,false,true,false); assert(!t.key_up(0xa0,false,true,false));
 t.set_enabled(false); assert(!tap(0xa0,0xa0));
 // A lost release on the other Shift must not disable future single taps.
 t.set_enabled(true); t.reset();
 t.key_down(0xa1,false,false,false); // right release was lost
 t.key_down(0xa0,false,false,false,false,false); // right was up when the left-down message was generated
 assert(t.would_toggle(0xa0,false,false,false));
 assert(t.key_up(0xa0,false,false,false));
 // A lost release of the same side must recover on the next fresh press.
 t.reset(); t.key_down(0xa0,false,false,false);
 t.key_down(0xa0,false,false,false,false,false);
 assert(t.key_up(0xa0,false,false,false));
 // A genuine two-Shift chord must still be rejected.
 t.reset(); t.key_down(0xa1,false,false,false);
 t.key_down(0xa0,false,false,false,false,true);
 assert(!t.key_up(0xa0,false,false,false));
 assert(!t.key_up(0xa1,false,false,false));
 assert(tap(0xa0,0xa0));
 // Auto-repeat cannot turn a held Shift into a new tap.
 t.reset(); t.key_down(0xa0,false,false,false);
 t.key_down('A',false,false,false);
 t.key_down(0xa0,false,false,false,true,false);
 assert(!t.key_up(0xa0,false,false,false));
 assert(tap(0xa0,0xa0));
 std::cout << "Shift tap, mixed key identities, chords and ambiguity passed\n";
}
