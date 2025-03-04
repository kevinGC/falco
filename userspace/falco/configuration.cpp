/*
Copyright (C) 2021 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <algorithm>

#include <list>
#include <set>

#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "falco_utils.h"

#include "configuration.h"
#include "logger.h"
#include "banned.h" // This raises a compilation error when certain functions are used

using namespace std;

falco_configuration::falco_configuration():
	m_buffered_outputs(false),
	m_time_format_iso_8601(false),
	m_webserver_enabled(false),
	m_webserver_listen_port(8765),
	m_webserver_k8s_audit_endpoint("/k8s-audit"),
	m_webserver_k8s_healthz_endpoint("/healthz"),
	m_webserver_ssl_enabled(false),
	m_config(NULL)
{
}

falco_configuration::~falco_configuration()
{
	if(m_config)
	{
		delete m_config;
	}
}

void falco_configuration::init(string conf_filename, const vector<string> &cmdline_options)
{
	string m_config_file = conf_filename;
	m_config = new yaml_configuration();
	try
	{
		m_config->load_from_file(m_config_file);
	}
	catch(const std::exception& e)
	{
		std::cerr << "Cannot read config file (" + m_config_file + "): " + e.what() + "\n";
		throw e;
	}

	init_cmdline_options(cmdline_options);

	list<string> rules_files;

	m_config->get_sequence<list<string>>(rules_files, string("rules_file"));

	for(auto &file : rules_files)
	{
		// Here, we only include files that exist
		struct stat buffer;
		if(stat(file.c_str(), &buffer) == 0)
		{
			read_rules_file_directory(file, m_rules_filenames);
		}
	}

	m_json_output = m_config->get_scalar<bool>("json_output", false);
	m_json_include_output_property = m_config->get_scalar<bool>("json_include_output_property", true);
	m_json_include_tags_property = m_config->get_scalar<bool>("json_include_tags_property", true);

	falco::outputs::config file_output;
	file_output.name = "file";
	if(m_config->get_scalar<bool>("file_output.enabled", false))
	{
		string filename, keep_alive;
		filename = m_config->get_scalar<string>("file_output.filename", "");
		if(filename == string(""))
		{
			throw logic_error("Error reading config file (" + m_config_file + "): file output enabled but no filename in configuration block");
		}
		file_output.options["filename"] = filename;

		keep_alive = m_config->get_scalar<string>("file_output.keep_alive", "");
		file_output.options["keep_alive"] = keep_alive;

		m_outputs.push_back(file_output);
	}

	falco::outputs::config stdout_output;
	stdout_output.name = "stdout";
	if(m_config->get_scalar<bool>("stdout_output.enabled", false))
	{
		m_outputs.push_back(stdout_output);
	}

	falco::outputs::config syslog_output;
	syslog_output.name = "syslog";
	if(m_config->get_scalar<bool>("syslog_output.enabled", false))
	{
		m_outputs.push_back(syslog_output);
	}

	falco::outputs::config program_output;
	program_output.name = "program";
	if(m_config->get_scalar<bool>("program_output.enabled", false))
	{
		string program, keep_alive;
		program = m_config->get_scalar<string>("program_output.program", "");
		if(program == string(""))
		{
			throw logic_error("Error reading config file (" + m_config_file + "): program output enabled but no program in configuration block");
		}
		program_output.options["program"] = program;

		keep_alive = m_config->get_scalar<string>("program_output.keep_alive", "");
		program_output.options["keep_alive"] = keep_alive;

		m_outputs.push_back(program_output);
	}

	falco::outputs::config http_output;
	http_output.name = "http";
	if(m_config->get_scalar<bool>("http_output.enabled", false))
	{
		string url;
		url = m_config->get_scalar<string>("http_output.url", "");

		if(url == string(""))
		{
			throw logic_error("Error reading config file (" + m_config_file + "): http output enabled but no url in configuration block");
		}
		http_output.options["url"] = url;

		string user_agent;
		user_agent = m_config->get_scalar<string>("http_output.user_agent","falcosecurity/falco");
		http_output.options["user_agent"] = user_agent;

		m_outputs.push_back(http_output);
	}

	m_grpc_enabled = m_config->get_scalar<bool>("grpc.enabled", false);
	m_grpc_bind_address = m_config->get_scalar<string>("grpc.bind_address", "0.0.0.0:5060");
	m_grpc_threadiness = m_config->get_scalar<uint32_t>("grpc.threadiness", 0);
	if(m_grpc_threadiness == 0)
	{
		m_grpc_threadiness = falco::utils::hardware_concurrency();
	}
	// todo > else limit threadiness to avoid oversubscription?
	m_grpc_private_key = m_config->get_scalar<string>("grpc.private_key", "/etc/falco/certs/server.key");
	m_grpc_cert_chain = m_config->get_scalar<string>("grpc.cert_chain", "/etc/falco/certs/server.crt");
	m_grpc_root_certs = m_config->get_scalar<string>("grpc.root_certs", "/etc/falco/certs/ca.crt");

	falco::outputs::config grpc_output;
	grpc_output.name = "grpc";
	// gRPC output is enabled only if gRPC server is enabled too
	if(m_config->get_scalar<bool>("grpc_output.enabled", true) && m_grpc_enabled)
	{
		m_outputs.push_back(grpc_output);
	}

	if(m_outputs.size() == 0)
	{
		throw logic_error("Error reading config file (" + m_config_file + "): No outputs configured. Please configure at least one output file output enabled but no filename in configuration block");
	}

	m_log_level = m_config->get_scalar<string>("log_level", "info");

	falco_logger::set_level(m_log_level);

	m_output_timeout = m_config->get_scalar<uint32_t>("output_timeout", 2000);

	m_notifications_rate = m_config->get_scalar<uint32_t>("outputs.rate", 1);
	m_notifications_max_burst = m_config->get_scalar<uint32_t>("outputs.max_burst", 1000);

	string priority = m_config->get_scalar<string>("priority", "debug");
	if (!falco_common::parse_priority(priority, m_min_priority))
	{
		throw logic_error("Unknown priority \"" + priority + "\"--must be one of emergency, alert, critical, error, warning, notice, informational, debug");
	}

	m_buffered_outputs = m_config->get_scalar<bool>("buffered_outputs", false);
	m_time_format_iso_8601 = m_config->get_scalar<bool>("time_format_iso_8601", false);

	falco_logger::log_stderr = m_config->get_scalar<bool>("log_stderr", false);
	falco_logger::log_syslog = m_config->get_scalar<bool>("log_syslog", true);

	m_webserver_enabled = m_config->get_scalar<bool>("webserver.enabled", false);
	m_webserver_listen_port = m_config->get_scalar<uint32_t>("webserver.listen_port", 8765);
	m_webserver_k8s_audit_endpoint = m_config->get_scalar<string>("webserver.k8s_audit_endpoint", "/k8s-audit");
	m_webserver_k8s_healthz_endpoint = m_config->get_scalar<string>("webserver.k8s_healthz_endpoint", "/healthz");
	m_webserver_ssl_enabled = m_config->get_scalar<bool>("webserver.ssl_enabled", false);
	m_webserver_ssl_certificate = m_config->get_scalar<string>("webserver.ssl_certificate", "/etc/falco/falco.pem");

	std::list<string> syscall_event_drop_acts;
	m_config->get_sequence(syscall_event_drop_acts, "syscall_event_drops.actions");

	for(std::string &act : syscall_event_drop_acts)
	{
		if(act == "ignore")
		{
			m_syscall_evt_drop_actions.insert(syscall_evt_drop_action::IGNORE);
		}
		else if(act == "log")
		{
			if(m_syscall_evt_drop_actions.count(syscall_evt_drop_action::IGNORE))
			{
				throw logic_error("Error reading config file (" + m_config_file + "): syscall event drop action \"" + act + "\" does not make sense with the \"ignore\" action");
			}
			m_syscall_evt_drop_actions.insert(syscall_evt_drop_action::LOG);
		}
		else if(act == "alert")
		{
			if(m_syscall_evt_drop_actions.count(syscall_evt_drop_action::IGNORE))
			{
				throw logic_error("Error reading config file (" + m_config_file + "): syscall event drop action \"" + act + "\" does not make sense with the \"ignore\" action");
			}
			m_syscall_evt_drop_actions.insert(syscall_evt_drop_action::ALERT);
		}
		else if(act == "exit")
		{
			m_syscall_evt_drop_actions.insert(syscall_evt_drop_action::EXIT);
		}
		else
		{
			throw logic_error("Error reading config file (" + m_config_file + "): available actions for syscall event drops are \"ignore\", \"log\", \"alert\", and \"exit\"");
		}
	}

	if(m_syscall_evt_drop_actions.empty())
	{
		m_syscall_evt_drop_actions.insert(syscall_evt_drop_action::IGNORE);
	}

	m_syscall_evt_drop_threshold = m_config->get_scalar<double>("syscall_event_drops.threshold", .1);
	if(m_syscall_evt_drop_threshold < 0 || m_syscall_evt_drop_threshold > 1)
	{
		throw logic_error("Error reading config file (" + m_config_file + "): syscall event drops threshold must be a double in the range [0, 1]");
	}
	m_syscall_evt_drop_rate = m_config->get_scalar<double>("syscall_event_drops.rate", .03333);
	m_syscall_evt_drop_max_burst = m_config->get_scalar<double>("syscall_event_drops.max_burst", 1);
	m_syscall_evt_simulate_drops = m_config->get_scalar<bool>("syscall_event_drops.simulate_drops", false);

	m_syscall_evt_timeout_max_consecutives = m_config->get_scalar<uint32_t>("syscall_event_timeouts.max_consecutives", 1000);
	if(m_syscall_evt_timeout_max_consecutives == 0)
	{
		throw logic_error("Error reading config file(" + m_config_file + "): the maximum consecutive timeouts without an event must be an unsigned integer > 0");
	}

	m_metadata_download_max_mb = m_config->get_scalar<uint32_t>("metadata_download.max_mb", 100);
	if(m_metadata_download_max_mb > 1024)
	{
		throw logic_error("Error reading config file(" + m_config_file + "): metadata download maximum size should be < 1024 Mb");
	}
	m_metadata_download_chunk_wait_us = m_config->get_scalar<uint32_t>("metadata_download.chunk_wait_us", 1000);
	m_metadata_download_watch_freq_sec = m_config->get_scalar<uint32_t>("metadata_download.watch_freq_sec", 1);
	if(m_metadata_download_watch_freq_sec == 0)
	{
		throw logic_error("Error reading config file(" + m_config_file + "): metadata download watch frequency seconds must be an unsigned integer > 0");
	}

	std::set<std::string> load_plugins;

	bool load_plugins_node_defined = m_config->is_defined("load_plugins");

	m_config->get_sequence<set<string>>(load_plugins, "load_plugins");

	std::list<falco_configuration::plugin_config> plugins;
	try
	{
		m_config->get_sequence<std::list<falco_configuration::plugin_config>>(plugins, string("plugins"));
	}
	catch (exception &e)
	{
		// Might be thrown due to not being able to open files
		throw logic_error("Error reading config file(" + m_config_file + "): could not load plugins config: " + e.what());
	}

	// If load_plugins was specified, only save plugins matching those in values
	for (auto &p : plugins)
	{
		// If load_plugins was not specified at all, every
		// plugin is added. Otherwise, the plugin must be in
		// the load_plugins list.
		if(!load_plugins_node_defined || load_plugins.find(p.m_name) != load_plugins.end())
		{
			m_plugins.push_back(p);
		}
	}
}

void falco_configuration::read_rules_file_directory(const string &path, list<string> &rules_filenames)
{
	struct stat st;

	int rc = stat(path.c_str(), &st);

	if(rc != 0)
	{
		std::cerr << "Could not get info on rules file " << path << ": " << strerror(errno) << std::endl;
		exit(-1);
	}

	if(st.st_mode & S_IFDIR)
	{
		// It's a directory. Read the contents, sort
		// alphabetically, and add every path to
		// rules_filenames
		vector<string> dir_filenames;

		DIR *dir = opendir(path.c_str());

		if(!dir)
		{
			std::cerr << "Could not get read contents of directory " << path << ": " << strerror(errno) << std::endl;
			exit(-1);
		}

		for(struct dirent *ent = readdir(dir); ent; ent = readdir(dir))
		{
			string efile = path + "/" + ent->d_name;

			rc = stat(efile.c_str(), &st);

			if(rc != 0)
			{
				std::cerr << "Could not get info on rules file " << efile << ": " << strerror(errno) << std::endl;
				exit(-1);
			}

			if(st.st_mode & S_IFREG)
			{
				dir_filenames.push_back(efile);
			}
		}

		closedir(dir);

		std::sort(dir_filenames.begin(),
			  dir_filenames.end());

		for(string &ent : dir_filenames)
		{
			rules_filenames.push_back(ent);
		}
	}
	else
	{
		// Assume it's a file and just add to
		// rules_filenames. If it can't be opened/etc that
		// will be reported later..
		rules_filenames.push_back(path);
	}
}

static bool split(const string &str, char delim, pair<string, string> &parts)
{
	size_t pos;

	if((pos = str.find_first_of(delim)) == string::npos)
	{
		return false;
	}
	parts.first = str.substr(0, pos);
	parts.second = str.substr(pos + 1);

	return true;
}

void falco_configuration::init_cmdline_options(const vector<string> &cmdline_options)
{
	for(const string &option : cmdline_options)
	{
		set_cmdline_option(option);
	}
}

void falco_configuration::set_cmdline_option(const string &opt)
{
	pair<string, string> keyval;

	if(!split(opt, '=', keyval))
	{
		throw logic_error("Error parsing config option \"" + opt + "\". Must be of the form key=val or key.subkey=val");
	}

	m_config->set_scalar(keyval.first, keyval.second);
}
