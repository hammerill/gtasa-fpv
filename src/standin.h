// The world reacts to the drone as if it were CJ (like sm64ex-fpv's Mario):
// once the drone is 20 m from CJ (the player's F8 > Fly > Pilot distances),
// CJ himself goes along with it, hidden,
// frozen, collision-less and untouchable, so peds, gangs, police, traffic,
// population and streaming all see "CJ" right where the quad is. Back under
// 10 m from his spot, he goes home. The player can turn it off there. Driving a vehicle, he gets out of it on
// the spot to stand in the same way, and back in behind the wheel afterwards;
// meanwhile his car is held at its spot while the drone is far (its ground's
// collision streams out) and kept from the population's clean-up. On a mission
// or as a passenger he stays in it, and only the FindPlayer* queries
// (streaming, population, traffic) follow the drone.
// He still shows at his spot or in his seat (pilot.cpp: moved there for the
// render passes).
#pragma once

namespace standin {

// Hooks (FindPlayer*, damage to CJ, CJ's ProcessControl). Once, game thread.
void install_hooks();

// Every drone step in flight (not in the pilot view) with the drone's
// position; home is where CJ was.
void update(const float drone_pos[3]);

// Ends it: CJ back to his spot (and vehicle) or, go_home false, left at the drone.
void end(bool go_home);

bool on();
void tick();  // every frame, the stand-in on or off

// Where CJ really is (his spot while he stands in), for the GPS home.
void home(float out[3]);

}  // namespace standin
