//
// Copyright 2015 KISS Technologies GmbH, Switzerland
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


#include "cpp-lib/sys/server.h"

#include "cpp-lib/sys/syslogger.h"

#include <exception>
#include <stdexcept>
#include <thread>

using namespace cpl::util          ;
using namespace cpl::util::log     ;
using namespace cpl::util::network ;


namespace {

std::string this_thread_id_paren() {
  return
      "(thread " 
    + boost::lexical_cast<std::string>(std::this_thread::get_id())
    + ")";
}

// A thread for each incoming connection
struct connection_thread {
  connection_thread(
      std::unique_ptr<cpl::util::network::connection> c,
      server_parameters const& params,
      input_handler_type const& handler,
      boost::optional<os_writer> const welcome)
    : c{std::move(c)},
      params{params},
      handler{handler},
      welcome{welcome}
  {}

  void operator()();

private:

  std::unique_ptr<connection> c;
  server_parameters params;
  input_handler_type handler;
  boost::optional<os_writer> welcome;

};


void handle_connection(
    std::ostream& sl, 
    boost::optional<os_writer> const welcome,
    input_handler_type const& handler,
    std::istream& is,
    std::ostream& os,
    long const max_line_length) {

  if (welcome) {
    (welcome.get())(os);
  }

  std::string line;
  while (cpl::util::getline(is, line, max_line_length)) {
    if (!handler(line, is, os, sl)) {
      break;
    }
  }
}

void connection_thread::operator()() { 
  syslogger sl{params.server_name + " conn " + this_thread_id_paren()};

  // Threads must not throw!
  try {

  instream is(*c);
  onstream os(*c);
  if (params.log_connections) {
    sl << prio::NOTICE << "New connection from " << c->peer() << std::endl;
  }

  handle_connection(sl, welcome, handler, is, os, params.max_line_length);

  // Timeout, EOF or error.  There's currently no good way to distinguish
  // these cases.
  if (params.log_connections) {
    sl << prio::NOTICE << "Connection closing: " << c->peer() << std::endl;
  }

  } catch (std::exception const& e) {
    sl << prio::ERR << e.what() << std::endl;
  }
}


struct server_thread {
  server_thread(
      acceptor&& a,
      input_handler_type const& handler,
      boost::optional<os_writer> const welcome,
      server_parameters params)
  : a(std::move(a)),
    handler(handler),
    welcome(welcome),
    params(params) {}

  void operator()();

private:
  acceptor a;
  input_handler_type handler;
  boost::optional<os_writer> welcome;
  server_parameters params;
};

void log_params(
    std::ostream& sl, 
    server_parameters const& params, 
    bool const production) {

  sl << prio::NOTICE << "Starting server: " << params.server_name << std::endl;
  sl << prio::NOTICE << "Mode: " 
     << (production ? "Production" : "Test") << std::endl;
  sl << prio::NOTICE << "Maximum backlog: " << params.backlog << std::endl;
  sl << prio::NOTICE << "Connection timeout [s]: " 
                     << params.timeout << std::endl;
  sl << prio::NOTICE << "Maximum line length: " 
                     << params.max_line_length << std::endl;
}

void server_thread::operator()() {
  syslogger sl{params.server_name + " listen " + this_thread_id_paren()};

  log_params(sl, params, true);

  sl << prio::NOTICE << "Listening for incoming connections on " 
                     << a.local() 
                     << std::endl;

  try {

  while (true) {
    std::unique_ptr<connection> c(new connection(a));
    c->timeout(params.timeout);
    std::thread t((connection_thread{std::move(c), params, handler, welcome}));
    t.detach();
  }

  } catch (std::exception const& e) {
    cpl::util::log::log_error(
        sl, "Aborting server " + params.server_name, e.what());
  }
}

} // end anonymous namespace


void cpl::util::run_server(
    input_handler_type const& handler,
    boost::optional<os_writer> const welcome,
    server_parameters const& params) {
  if ("test:stdio" == params.service) {
    try {
      log_params(std::cout, params, false);
      handle_connection(
          std::cout, welcome, handler, 
          std::cin, std::cout, params.max_line_length);
    } catch (std::exception const& e) {
      cpl::util::log::log_error(
          std::cout, 
          "Aborting test mode server " + params.server_name, e.what());
    }
  } else {
    server_thread st(
        acceptor(params.service, params.backlog), handler, welcome, params);

    if (params.background) {
      std::thread t(std::move(st));
      t.detach();
    } else {
      st();
    }
  }
}
