#include "stdio.h"
#include "boson/boson.h"
#include "boson/channel.h"
#include "boson/select.h"
#include <unistd.h>
#include <string>
#include <iostream>

using namespace std::literals;

int main(int argc, char *argv[]) {
  // Create a pipe
  int pipe_fds[2];
  ::pipe(pipe_fds);
  ::fcntl(pipe_fds[0], F_SETFL, ::fcntl(pipe_fds[0], F_GETFD) | O_NONBLOCK);
  ::fcntl(pipe_fds[1], F_SETFL, ::fcntl(pipe_fds[1], F_GETFD) | O_NONBLOCK);

  boson::run(1, [&]() {
    using namespace boson;

    // Create channel
    channel<int, 3> chan;

    // Create mutex and lock it immediately
    boson::mutex mut;
    mut.lock();

    // Start a producer
    start([](int out, auto chan) -> void {
        int data = 1;
        boson::write(out, &data, sizeof(data));
        chan << data;
    }, pipe_fds[1], chan);

    // Start a consumer
    start([](int in, auto chan, auto mut) -> void {
        int buffer = 0;
        bool stop = false;
        while (!stop) {
          select_any(  //
              event_read(in, &buffer, sizeof(buffer),
                         [](ssize_t rc) {  //
                           std::cout << "Got data from the pipe \n";
                         }),
              event_read(chan, buffer,
                         [](bool) {  //
                           std::cout << "Got data from the channel \n";
                         }),
              event_lock(mut,
                         []() {  //
                           std::cout << "Got lock on the mutex \n";
                         }),
              event_timer(100ms,
                          [&stop]() {  //
                            std::cout << "Nobody loves me anymore :(\n";
                            stop = true;
                          }));
        }
    },  pipe_fds[0], chan, mut);
    
    // Start an unlocker
    start([](auto mut) -> void {
      mut.unlock();
    }, mut);
  });

  boson::run(1, [&]() {
    using namespace boson;

    // Create channel
    channel<int, 3> chan;

    // Start a producer
    start([](int out, auto chan) -> void {
        int data = 1;
        boson::write(out, &data, sizeof(data));
        chan << data;
    }, pipe_fds[1], chan);

    // Start a consumer
    start([](int in, auto chan) -> void {
        int buffer = 0;
        bool stop = false;
        while (!stop) {
          int result = select_any(                                                    //
              event_read(in, &buffer, sizeof(buffer), [](ssize_t rc) { return 1; }),  //
              event_read(chan, buffer, [](bool) { return 2; }),                           //
              event_timer(100ms, []() { return 3; }));                                //
          switch(result) {
            case 1:
              std::cout << "Got data from the pipe \n";
              break;
            case 2:
              std::cout << "Got data from the channel \n";
              break;
            default:
              std::cout << "Nobody loves me anymore :(\n";
              stop = true;
              break;
          }
        }
    },  pipe_fds[0], chan);
  });
}
