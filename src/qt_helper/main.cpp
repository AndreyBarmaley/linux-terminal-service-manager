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

#include <QApplication>
#include <QTranslator>

#include "ltsm_application.h"
#include "ltsm_helperwindow.h"

int main( int argc, char* argv[] )
{
    QApplication app( argc, argv );
    QTranslator tr;
    tr.load( QLocale(), QLatin1String( "ltsm_helper" ), QLatin1String( "_" ), QLatin1String( ":/i18n" ) );
    app.installTranslator( & tr );

    try
    {

        LTSM::LoginHelper::ManagerServiceProxy win;
        win.show();
        return app.exec();

    }
    catch( const sdbus::Error & err )
    {
        LTSM::Application::error( "sdbus exception: [%s] %s", err.getName().c_str(), err.getMessage().c_str() );
    }
    catch( const std::exception & err )
    {
        LTSM::Application::error( "%s: exception: %s", NS_FuncName.c_str(), err.what() );
    }

    return EXIT_FAILURE;
}
