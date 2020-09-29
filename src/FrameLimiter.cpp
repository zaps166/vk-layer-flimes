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

#include "FrameLimiter.hpp"

#include <thread>

using namespace std;

FrameLimiter::FrameLimiter(const double fps)
{
    m_delay = (fps > 0.0)
        ? duration(static_cast<duration::rep>(duration::period::den / fps / duration::period::num))
        : duration::zero();
    m_timePoint = frame_clock::now().time_since_epoch();
}
FrameLimiter::~FrameLimiter()
{
}

void FrameLimiter::wait()
{
    if (m_delay == duration::zero())
        return;

    const duration newTimePoint = frame_clock::now().time_since_epoch();
    const duration sleepTime = m_delay - newTimePoint + m_timePoint;

    m_timePoint = newTimePoint;

    if (sleepTime.count() > 0)
    {
        this_thread::sleep_for(sleepTime);
        m_timePoint += sleepTime;
    }
}
