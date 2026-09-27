#include <pawndb/lifecycle.hpp>

int main() {
  pawndb::Lifecycle lifecycle;
  int script = 0;
  if (lifecycle.attach(&script)) return 1;
  lifecycle.start();
  if (!lifecycle.attach(&script) || lifecycle.attach(&script) || lifecycle.script_count() != 1)
    return 1;
  lifecycle.dispatch_tick();
  lifecycle.detach(&script);
  if (lifecycle.script_count() != 0) return 1;
  lifecycle.attach(&script);
  lifecycle.stop();
  return lifecycle.script_count() == 0 && !lifecycle.attach(&script) ? 0 : 1;
}
