/*
    MIT License

    Copyright (c) 2020 Błażej Szczygieł

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "ExternalControl.hpp"

#include <iostream>
#include <cstring>

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

using namespace std;

ExternalControl::ExternalControl(const Fn &fn)
    : m_processExternalCommandFn(fn)
{
    m_path = filesystem::temp_directory_path().append(VK_LAYER_FLIMES_NAME);

    auto fileName = strrchr(program_invocation_name, '\\');
    if (!fileName)
        fileName = strrchr(program_invocation_name, '/');
    if (!fileName)
        fileName = program_invocation_name;
    else
        ++fileName;

    m_fifoPath = filesystem::path(m_path).append(fileName).concat("-").concat(to_string(getpid()));

    error_code e;
    filesystem::create_directories(m_path, e);
    if (e)
    {
        cerr << "  Can't create external control directory: " << e << "\n";
        return;
    }

    m_efd = eventfd(0, 0);
    if (m_efd < 0)
    {
        return;
    }

    mkfifo(m_fifoPath.c_str(), 0600);
    if (!filesystem::is_fifo(m_fifoPath))
    {
        cerr << "  Can't create external control pipe: " << m_fifoPath << "\n";
        return;
    }

    cout << "  External control enabled: " << m_fifoPath << "\n";

    m_thr = thread(bind(&ExternalControl::run, this));
}
ExternalControl::~ExternalControl()
{
    if (m_thr.joinable())
    {
        eventfd_write(m_efd, 1); // abort "pool()"

        m_thr.join();

        filesystem::remove(m_fifoPath);

        try
        {
            filesystem::remove(m_path);
        }
        catch (const filesystem::filesystem_error &)
        {}
    }
    if (m_efd > -1)
    {
        close(m_efd);
    }
}

void ExternalControl::run()
{
    int fd = -1;
    string str;

    for (;;)
    {
        if (fd < 0)
        {
            fd = open(m_fifoPath.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0)
            {
                cerr << "External control error" << endl;
                break;
            }
        }

        pollfd fds[2] = {
            {
                .fd = m_efd,
                .events = POLLIN,
                .revents = 0,
            },
            {
                .fd = fd,
                .events = POLLIN,
                .revents = 0,
            },
        };
        if (poll(fds, 2, -1) < 0)
            return;

        // Check for app exit
        if (fds[0].revents & POLLIN)
        {
            eventfd_t val = 0;
            eventfd_read(m_efd, &val);
            break;
        }

        // Check for external commands
        if (fds[1].revents & POLLIN)
        {
            char c;
            while (read(fd, &c, 1) == 1)
            {
                if (c != '\n' && c != ' ')
                {
                    str += toupper(c);
                    continue;
                }

                m_processExternalCommandFn(str);
                str.clear();
            }
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            close(fd);
            fd = -1;
        }
    }

    if (fd > -1)
    {
        close(fd);
    }
}
