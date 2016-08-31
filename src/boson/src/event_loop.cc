#include "boson/event_loop.h"
#include "event_loop_impl.h"

namespace boson {

event_loop::~event_loop() {
}

event_loop::event_loop(event_handler& handler) :
  loop_impl_{new event_loop_impl{handler}}
{
}

int event_loop::register_event(void *data) {
  return loop_impl_->register_event(data);
}

void event_loop::send_event(int event) {
  loop_impl_->send_event(event);
}

void event_loop::loop(int max_iter) {
  loop_impl_->loop(max_iter);
}

}  // namespace boson
