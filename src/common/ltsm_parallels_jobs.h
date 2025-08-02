/***************************************************************************
 *   Copyright © 2025 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _LTSM_PARALLELS_JOBS_
#define _LTSM_PARALLELS_JOBS_

#include <list>
#include <thread>
#include <chrono>
#include <future>

using namespace std::chrono_literals;

namespace LTSM
{
    template<typename JobResult>
    class ParallelsJobs
    {
        using JobFuture = std::future<JobResult>;
        using JobList = std::list<JobFuture>;

        JobList jobs;
        const int tnum;

    public:
        explicit ParallelsJobs(int num = std::thread::hardware_concurrency()) : tnum(num)
        {
        }

        ~ParallelsJobs()
        {
            for(auto & job: jobs)
                if(job.valid()) job.wait();
        }

        void addJob(JobFuture && job)
        {
            while(! jobs.empty())
            {
                auto busy = std::count_if(jobs.begin(), jobs.end(), [](auto & job)
                {
                    return job.wait_for(1us) != std::future_status::ready;
                });

                if(busy < tnum)
                    break;
            }

            jobs.emplace_back(std::forward<JobFuture>(job));
        }

        JobList & waitAll(void)
        {
            for(auto & job: jobs)
                job.wait();

            return jobs;
        }

        JobList & jobList(void)
        {
            return jobs;
        }
        
        void clear(void)
        {
            waitAll().clear();
        }
    };
}

#endif
