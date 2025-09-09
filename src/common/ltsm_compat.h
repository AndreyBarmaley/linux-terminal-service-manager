/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
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

#ifndef _LTSM_COMPAT_
#define _LTSM_COMPAT_


#include <string>

namespace LTSM
{
    inline std::string view2string(std::string_view view)
    {
        return std::string(view.begin(), view.end());
    }

    template<typename Iter>
    inline std::string_view string2view(Iter it1, Iter it2)
    {
#if __cplusplus >= 202002L
        return std::string_view{it1, it2};
#else
        return std::string_view{std::addressof(*it1), (size_t) (it2 - it1) };
#endif
    }

    inline bool startsWith(std::string_view str, std::string_view pred)
    {
	return pred.size() <= str.size() &&
	    str.substr(0, pred.size()) == pred;
    }

    inline bool endsWith(std::string_view str, std::string_view pred)
    {
	return pred.size() <= str.size() &&
	    str.substr(str.size() - pred.size(), pred.size()) == pred;
    }
}

#endif // _LTSM_COMPAT_
