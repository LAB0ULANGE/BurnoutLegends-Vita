#pragma once

#include <functional>

// Runs the SAS synthesizer (sas_core) on its own thread, so mixing a grain
// overlaps the game instead of stopping it: __sceSasCore and the voice
// setters only queue their work, and whatever reads the synthesizer's state
// (end flags, envelope heights) first waits for the queue to empty. The
// emulation also waits at every vblank, so guest memory (the mixed grains) is
// the same at each vblank as with the synchronous mixer.
namespace burnout::sas_worker {

// Starts the thread (Vita: on `core`). Returns false if it could not.
bool start(int core);
[[nodiscard]] bool active() noexcept;

// Runs `work` on the worker after everything posted before it, or at once
// when the worker is not running.
void post(std::function<void()> work);
// Waits until everything posted so far has run.
void drain();

// Host time the worker spent running work, in ms, since the last call.
[[nodiscard]] double take_busy_ms();

} // namespace burnout::sas_worker
