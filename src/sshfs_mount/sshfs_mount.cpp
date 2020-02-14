/*
 * Copyright (C) 2017-2019 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <multipass/sshfs_mount/sshfs_mount.h>

#include <multipass/exceptions/sshfs_missing_error.h>
#include <multipass/logging/log.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/sshfs_mount/sftp_server.h>
#include <multipass/utils.h>

#include <multipass/format.h>

#include <iostream>
#include <vector>
#include <filesystem>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "sshfs mount";
template <typename Callable>
auto run_cmd(mp::SSHSession& session, std::string&& cmd, Callable&& error_handler)
{
    auto ssh_process = session.exec(cmd);
    if (ssh_process.exit_code() != 0)
        error_handler(ssh_process);
    return ssh_process.read_std_output();
}

// Run a command on a given SSH session.
auto run_cmd(mp::SSHSession& session, std::string&& cmd)
{
    auto error_handler = [](mp::SSHProcess& proc) { throw std::runtime_error(proc.read_std_error()); };
    return run_cmd(session, std::forward<std::string>(cmd), error_handler);
}

// Check if sshfs exists on a given SSH session.
void check_sshfs_exists(mp::SSHSession& session)
{
    auto error_handler = [](mp::SSHProcess& proc) {
        mpl::log(mpl::Level::warning, category,
                 fmt::format("Unable to determine if 'sshfs' is installed: {}", proc.read_std_error()));
        throw mp::SSHFSMissingError();
    };

    run_cmd(session, "which sshfs", error_handler);
}

void make_target_dir(mp::SSHSession& session, const std::string& target)
{
    run_cmd(session, fmt::format("sudo mkdir -p \"{}\"", target));
}

// Split a path string in pieces.
void split_path(const std::string& path, std::vector<std::string>& splitting)
{
    std::string dir;

    for (char c: path)
    {
        if (('\\' == c || '/' == c) && !dir.empty())
        {
            splitting.push_back(dir);
        }
        dir.push_back('\\' == c ? '/' : c);
    }

    splitting.push_back(dir);

    return;
}

// Set ownership of a directory. Assume it is already created.
void set_owner_for(mp::SSHSession& session, const std::string& target)
{
    auto vm_user = run_cmd(session, "id -nu");
    auto vm_group = run_cmd(session, "id -ng");
    mp::utils::trim_end(vm_user);
    mp::utils::trim_end(vm_group);

    run_cmd(session, fmt::format("sudo chown {}:{} \"{}\"", vm_user, vm_group, target));
}

auto make_sftp_server(mp::SSHSession&& session, const std::string& source, const std::string& target,
                      const std::unordered_map<int, int>& gid_map, const std::unordered_map<int, int>& uid_map)
{
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(source = {}, target = {}, …): ", __FILE__, __LINE__, __FUNCTION__, source, target));

    check_sshfs_exists(session);

    std::vector<std::string> splitting;
    split_path(target, splitting);
    bool needs_create = false;
    for (std::string partial: splitting)
    {
        if (needs_create || !std::filesystem::exists(partial))
        {
            needs_create = true;
            make_target_dir(session, partial);
            set_owner_for(session, partial);
        }
    }

    auto output = run_cmd(session, "id -u");
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(): `id -u` = {}", __FILE__, __LINE__, __FUNCTION__, output));
    auto default_uid = std::stoi(output);
    output = run_cmd(session, "id -g");
    mpl::log(mpl::Level::debug, category,
             fmt::format("{}:{} {}(): `id -g` = {}", __FILE__, __LINE__, __FUNCTION__, output));
    auto default_gid = std::stoi(output);

    return std::make_unique<mp::SftpServer>(std::move(session), source, target, gid_map, uid_map, default_uid,
                                            default_gid);
}

} // namespace

mp::SshfsMount::SshfsMount(SSHSession&& session, const std::string& source, const std::string& target,
                           const std::unordered_map<int, int>& gid_map, const std::unordered_map<int, int>& uid_map)
    : sftp_server{make_sftp_server(std::move(session), source, target, gid_map, uid_map)}, sftp_thread{[this] {
          std::cout << "Connected" << std::endl;
          sftp_server->run();
          std::cout << "Stopped" << std::endl;
      }}
{
}

mp::SshfsMount::~SshfsMount()
{
    stop();
}

void mp::SshfsMount::stop()
{
    sftp_server->stop();
    if (sftp_thread.joinable())
        sftp_thread.join();
}
