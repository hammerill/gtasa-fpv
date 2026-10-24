// The world reacts to the drone as if it were CJ (like sm64ex-fpv's Mario):
// once the drone is 20 m from CJ, CJ himself goes along with it, hidden,
// frozen, collision-less and untouchable, so peds, gangs, police, traffic,
// population and streaming all see "CJ" right where the quad is. Back under
// 10 m from his spot, he goes home. In a vehicle he stays put and only the
// FindPlayer* queries (streaming, population, traffic) follow the drone.
#pragma once

namespace standin {

// Hooks (FindPlayer*, damage to CJ, CJ's ProcessControl). Once, game thread.
void install_hooks();

// Every drone step with the drone's position; home is where CJ was.
void update(const float drone_pos[3]);

// Ends it: CJ back to his spot (and vehicle) or, go_home false, left at the drone.
void end(bool go_home);

bool on();

// Where CJ really is (his spot while he stands in), for the GPS home.
void home(float out[3]);

}  // namespace standin
