#include "event_loop_impl.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include "exception.h"
#include "system.h"
#include "logger.h"

template class std::unique_ptr<boson::event_loop>;

namespace boson {
event_loop::fd_data& event_loop::get_fd_data(int fd) {
  size_t index = static_cast<size_t>(fd);
  if (fd_data_.size() <= index) {
    fd_data_.resize(index+1);
  }
  return fd_data_[index];
}

void event_loop::epoll_update(int fd, fd_data& fddata, bool del_if_no_event) {
  if (events_.size() < events_data_.data().size()) events_.resize(events_data_.data().size());
  epoll_event_t new_event{(0 <= fddata.idx_read ? EPOLLIN : 0) |
                              (0 <= fddata.idx_write ? EPOLLOUT : 0) | EPOLLET | EPOLLRDHUP,
                          {}};
  new_event.data.fd = fd;
  int return_code = -1;
  if (0 != new_event.events) {
    return_code = ::epoll_ctl(loop_fd_, EPOLL_CTL_MOD, fd, &new_event);
    if (return_code < 0) {
      if (ENOENT == errno) {
        errno = 0;
        return_code = ::epoll_ctl(loop_fd_, EPOLL_CTL_ADD, fd, &new_event);
      } else if (EBADF == errno) {
        // Dispatch panic
        if (0 <= fddata.idx_read)
          dispatch_event(fddata.idx_read, -EBADF);
        if (0 <= fddata.idx_write)
          dispatch_event(fddata.idx_write, -EBADF);
      }
      else {
        throw exception(std::string("Syscall error (epoll_ctl): ") + ::strerror(errno));
      }
    }
  }
}

void event_loop::dispatch_event(int event_id, event_status status) {
  auto& data = events_data_[event_id];
  switch (data.type) {
    case event_type::event_fd: {
      size_t buffer{0};
      ssize_t nb_bytes = ::read(data.fd, &buffer, 8u);
      assert(nb_bytes == 8);
      if (loop_breaker_event_ == event_id) {
        // Empty the queue and send panics
        broken_loop_event_data* data = nullptr;
        while((data = static_cast<decltype(data)>(loop_breaker_queue_.read(0)))) {
          // We just ignore fds we dont have
          if (static_cast<size_t>(data->fd) < fd_data_.size()) {
            auto& fddata = get_fd_data(data->fd);
            if (0 <= fddata.idx_read)
              dispatch_event(fddata.idx_read, -EINTR);
            if (0 <= fddata.idx_write)
              dispatch_event(fddata.idx_write, -EINTR);
          }
          delete data;
        }
      }
      else {
        handler_.event(event_id, data.data, status);
      }
    } break;
    case event_type::read: {
      handler_.read(data.fd, data.data, status);
    } break;
    case event_type::write: {
      handler_.write(data.fd, data.data, status);
    } break;
  }
}

event_loop::event_loop(event_handler& handler, int nprocs)
    : handler_{handler},
      loop_fd_{epoll_create1(0)},
      nb_io_registered_(0),
      trigger_fd_events_{false},
      loop_breaker_event_{-1},
      loop_breaker_queue_{nprocs+1} {
  loop_breaker_event_ = register_event(nullptr);
}

event_loop::~event_loop() {
  ::close(loop_fd_);
}

int event_loop::register_event(void* data) {
  // Creates an eventfd
  int event_fd = ::eventfd(0, 0);

  // Creates users data
  size_t event_id = static_cast<int>(events_data_.allocate());
  event_data& ev_data = events_data_[event_id];
  ev_data.fd = event_fd;
  ev_data.type = event_type::event_fd;
  ev_data.data = data;
  auto& fddata = get_fd_data(event_fd);
  fddata.idx_read = event_id;
  noio_events_.insert(event_id);

  // Register
  epoll_update(event_fd,fddata,false);

  // Casted to int, we dont have to worry about scaling here, int is waaaaay large enough
  return event_id;
}

void* event_loop::get_data(int event_id) {
  return  (0 <= event_id) ?  events_data_[event_id].data : nullptr;
}

std::tuple<int,int> event_loop::get_events(int fd) {
  if (static_cast<size_t>(fd) < fd_data_.size()) {
    auto& data = get_fd_data(fd);
    return std::make_tuple(data.idx_read, data.idx_write);
  }
  return std::make_tuple(-1,-1);
}

void event_loop::send_event(int event) {
  auto& data = events_data_[static_cast<size_t>(event)];
  size_t buffer{1};
  ssize_t nb_bytes = ::write(data.fd, &buffer, 8u);
  if (nb_bytes < 0) {
    throw exception(std::string("Syscall error (write): ") + strerror(errno));
  }
  trigger_fd_events_.store(true, std::memory_order_release);
}

int event_loop::register_read(int fd, void* data) {
  // Creates users data
  size_t event_id = static_cast<int>(events_data_.allocate());
  event_data& ev_data = events_data_[event_id];
  ev_data.fd = fd;
  ev_data.type = event_type::read;
  ev_data.data = data;
  auto& fddata = get_fd_data(fd);
  fddata.idx_read = event_id;

  // Register in epoll loop
  epoll_update(fd,fddata,false);

  ++nb_io_registered_;
  return event_id;
}

int event_loop::register_write(int fd, void* data) {
  // Creates users data
  size_t event_id = static_cast<int>(events_data_.allocate());
  event_data& ev_data = events_data_[event_id];
  ev_data.fd = fd;
  ev_data.type = event_type::write;
  ev_data.data = data;
  auto& fddata = get_fd_data(fd);
  fddata.idx_write = event_id;

  // Register in epoll loop
  epoll_update(fd,fddata,false);

  ++nb_io_registered_;
  return event_id;
}

void* event_loop::unregister(int event_id) {
  auto& event_data = events_data_[event_id];
  auto& fddata = get_fd_data(event_data.fd);
  if (event_data.type == event_type::read || event_data.type == event_type::event_fd)
    fddata.idx_read = -1;
  else if (event_data.type == event_type::write)
    fddata.idx_write = -1;

  //epoll_update(event_data.fd, fddata,true);
  epoll_update(event_data.fd, fddata, false);
  if (event_data.type == event_type::event_fd)
    noio_events_.erase(event_id);
  else
    --nb_io_registered_;
  void* data = event_data.data;
  events_data_.free(event_id);
  return data;
}

void event_loop::send_fd_panic(int proc_from,int fd) {
  loop_breaker_queue_.write(proc_from+1, new broken_loop_event_data{fd});
  send_event(loop_breaker_event_);
}

loop_end_reason event_loop::loop(int max_iter, int timeout_ms) {
  bool forever = (-1 == max_iter);
  bool retry = false;
  for (size_t index = 0; index < static_cast<size_t>(max_iter) || forever || retry; ++index) {
    int return_code = 0;
    retry = false;
    if (0 != timeout_ms || 0 < nb_io_registered_ ||
        trigger_fd_events_.load(std::memory_order_acquire)) {
      trigger_fd_events_.store(false, std::memory_order_relaxed);
      return_code = ::epoll_wait(loop_fd_, events_.data(), events_.size(), timeout_ms);
      switch (return_code) {
        case 0:
          if (timeout_ms != 0)
            return loop_end_reason::timed_out;
          else
            break;
        case EINTR:
          //throw exception(std::string("Syscall error (epoll_wait) EINTR : ") + ::strerror(errno));
          // TODO: real cause to be determined, happens under high contention
          retry = true;
          break;
        case EBADF:
          //throw exception(std::string("Syscall error (epoll_wait) EBADF : ") + std::to_string(loop_fd_) + ::strerror(errno));
          retry = true;
          break;
        case EFAULT:
          throw exception(std::string("Syscall error (epoll_wait) EFAULT : ") + ::strerror(errno));
        case EINVAL:
          throw exception(std::string("Syscall error (epoll_wait) EINVAL : ") + ::strerror(errno));
        default:
          break;
      }
      // Success, get on on with dispatching events
      for (int index = 0; index < return_code; ++index) {
        auto& epoll_event = events_[index];
        auto& fddata = get_fd_data(epoll_event.data.fd);
        if (epoll_event.events & (EPOLLERR | EPOLLRDHUP)) {
          //boson::debug::log("Yeah HANG UP {}", epoll_event.data.fd);
          if (0 < fddata.idx_read && !(epoll_event.events & EPOLLIN))
            dispatch_event(fddata.idx_read, -EINTR);
          if (0 < fddata.idx_write)
            dispatch_event(fddata.idx_write, -EINTR);
        }
        else if (epoll_event.events & EPOLLOUT) {
          dispatch_event(fddata.idx_write, 0);
        }
        if (epoll_event.events & EPOLLIN)
          dispatch_event(fddata.idx_read, 0);
      }
    }
  }
  return loop_end_reason::max_iter_reached;
}
}
