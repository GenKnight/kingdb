// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.
//
#include <execinfo.h>
#include <csignal>

#include "network/server.h"
#include "thread/threadpool.h"
#include "util/logger.h"
#include "util/options.h"
#include "util/file.h"
#include "util/config_parser.h"

static const uint32_t kVersionServerMajor    = 0;
static const uint32_t kVersionServerMinor    = 9;
static const uint32_t kVersionServerRevision = 0;
static const uint32_t kVersionServerBuild    = 0;

bool stop_requested = false;

void termination_signal_handler(int signal) {
  kdb::log::info("KingServer", "Received signal [%d]\n", signal);
  stop_requested = true; 
}

void crash_signal_handler(int sig) {
  int depth_max = 20;
  void *array[depth_max];
  size_t depth;

  depth = backtrace(array, depth_max);
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, depth, STDERR_FILENO);
  exit(1);
}


int daemonize() {
  // Adapted from Michael Kerrisk's becomeDaemon()
  
  kdb::FileUtil::kingdb_getcwd(); // cache the current working directory
                                  // before the fork() happens, otherwise
                                  // getcwd() returns "/" on Mac OS X.

  // Become background process
  switch (fork()) {
    case -1: return -1;
    case 0:  break;
    default: _exit(EXIT_SUCCESS);
  }

  if (setsid() == -1) return -1;

  // Ensure we are not session leader
  switch (fork()) {
    case -1: return -1;
    case 0:  break;
    default: _exit(EXIT_SUCCESS);
  }

  umask(0);
  if (chdir("/") < 0) {
    fprintf(stderr, "chdir(): %s\n", strerror(errno));
  }

  return 0; 
}


int main(int argc, char** argv) {

  kdb::Status s;
  std::string dbname = "";
  std::string configfile = "";
  bool run_in_foreground = false;
  kdb::ServerOptions server_options;
  kdb::DatabaseOptions db_options;

  // Looking for '--configfile'
  kdb::ConfigParser parser_configfile;
  parser_configfile.error_if_unknown_parameters = false;
  parser_configfile.AddParameter(new kdb::StringParameter(
                                 "configfile", "", &configfile, false,
                                 "Configuration file. If not specified, the path ./kingdb.conf and /etc/kingdb.conf will be tested."));

  s = parser_configfile.ParseCommandLine(argc, argv);
  if (!s.IsOK()) {
    fprintf(stderr, "%s\n", s.ToString().c_str());
    exit(-1);
  }
  
  struct stat info;
  if (configfile == "") {
    if (stat("./kingdb.conf", &info) == 0) {
      configfile = "./kingdb.conf";
    } else if (stat("/etc/kingdb.conf", &info) == 0) {
      configfile = "/etc/kingdb.conf";
    }
  } else if (stat(configfile.c_str(), &info) != 0) {
    fprintf(stderr, "Could not file configuration file [%s]\n", configfile.c_str());
    exit(-1);
  }

  // Now parsing all options
  kdb::ConfigParser parser;

  // General options
  parser.AddParameter(new kdb::StringParameter(
                      "configfile", configfile, &configfile, false,
                      "Configuration file. If not specified, the path ./kingdb.conf and /etc/kingdb.conf will be tested."));
  parser.AddParameter(new kdb::FlagParameter(
                      "foreground", &run_in_foreground, false,
                      "When set, the server will run as a foreground process. By default, the server runs as a daemon process."));
  parser.AddParameter(new kdb::StringParameter(
                      "db.path", "", &dbname, true,
                      "Path where the database can be found or will be created."));

  kdb::DatabaseOptions::AddParametersToConfigParser(db_options, parser);
  kdb::ServerOptions::AddParametersToConfigParser(server_options, parser);

  // Overwrite the default value for the WriteBuffer mode
  parser.SetDefaultValue("db.write-buffer.mode", "adaptive");

  if (argc == 2 && (strncmp(argv[1], "--help", 6) == 0 || strncmp(argv[1], "-h", 2) == 0)) {
    fprintf(stdout, "KingServer is a persisted key-value database server, which uses the KingDB library\nas a storage backend. For more information, visit http://kingdb.org\n");
    fprintf(stdout, "KingServer version: %d.%d.%d-%d\nKingDB version: %d.%d.%d\nData format version: %d.%d\n", kVersionServerMajor, kVersionServerMinor, kVersionServerRevision, kVersionServerBuild, kdb::kVersionMajor, kdb::kVersionMinor, kdb::kVersionRevision, kdb::kVersionDataFormatMajor, kdb::kVersionDataFormatMinor);
    fprintf(stdout, "\nParameters:\n\n");
    parser.PrintUsage();
    exit(0);
  }

  if (argc == 2 && (strncmp(argv[1], "--generate-doc", 6) == 0 || strncmp(argv[1], "-h", 2) == 0)) {
    fprintf(stdout, "Generating the parameter list in markdown format for use in the documentation.\n\n");
    parser.PrintMarkdown();
    exit(0);
  }


  if (configfile != "") {
    s = parser.ParseFile(configfile); 
    if (!s.IsOK()) {
      fprintf(stderr, "%s\n", s.ToString().c_str());
      exit(-1);
    }
  }

  s = parser.ParseCommandLine(argc, argv);
  if (!s.IsOK()) {
    fprintf(stderr, "%s\n", s.ToString().c_str());
    exit(-1);
  }

  if (!parser.FoundAllMandatoryParameters()) {
    parser.PrintAllMissingMandatoryParameters();
    exit(-1);
  }

  if (db_options.log_level != "" && kdb::Logger::set_current_level(db_options.log_level.c_str()) < 0) {
    fprintf(stderr, "Unknown log level: [%s]\n", db_options.log_level.c_str());
    exit(-1);
  }

  kdb::Logger::set_target(db_options.log_target);
 
  kdb::CompressionType ctype;
  if (db_options.storage__compression_algorithm == "disabled") {
    ctype = kdb::kNoCompression;
  } else if (db_options.storage__compression_algorithm == "lz4") {
    ctype = kdb::kLZ4Compression;
  } else {
    fprintf(stderr, "Unknown compression algorithm: [%s]\n", db_options.storage__compression_algorithm.c_str());
    exit(-1);
  }
  db_options.compression = ctype;

  kdb::HashType htype;
  if (db_options.storage__hashing_algorithm == "xxhash-64") {
    htype = kdb::kxxHash_64;
  } else if (db_options.storage__hashing_algorithm == "murmurhash3-64") {
    htype = kdb::kMurmurHash3_64;
  } else {
    fprintf(stderr, "Unknown hashing algorithm: [%s]\n", db_options.storage__hashing_algorithm.c_str());
    exit(-1);
  }
  db_options.hash = htype;

  kdb::WriteBufferMode wbm;
  if (db_options.write_buffer__mode_str == "direct") {
    wbm = kdb::kWriteBufferModeDirect;
  } else if (db_options.write_buffer__mode_str == "adaptive") {
    wbm = kdb::kWriteBufferModeAdaptive;
  } else {
    fprintf(stderr, "Unknown write buffer mode: [%s]\n", db_options.write_buffer__mode_str.c_str());
    exit(-1);
  }
  db_options.write_buffer__mode = wbm;

  kdb::FileUtil::increase_limit_open_files();

#ifndef DEBUG
#endif
  signal(SIGINT, termination_signal_handler);
  signal(SIGTERM, termination_signal_handler);

  signal(SIGSEGV, crash_signal_handler);
  signal(SIGABRT, crash_signal_handler);

  if (!run_in_foreground && daemonize() < 0) {
    fprintf(stderr, "Could not daemonize the process\n");
    exit(-1);
  }

  kdb::Server server;
  server.Start(server_options, db_options, dbname);
  kdb::log::info("KingServer", "Daemon has started");
  while (!stop_requested && !server.IsStopRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  server.Stop();
  kdb::log::info("KingServer", "Daemon has stopped");
  return 0;
}
