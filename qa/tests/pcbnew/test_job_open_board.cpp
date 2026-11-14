/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <qa_utils/wx_utils/unit_test_utils.h>
#include <qa_utils/file_utils.h>
#include <pcbnew_utils/board_file_utils.h>

#include <board.h>
#include <board_loader.h>
#include <cli/exit_codes.h>
#include <footprint.h>
#include <jobs/job_export_pcb_pos.h>
#include <pcb_io/pcb_io_mgr.h>
#include <pcbnew_jobs_handler.h>
#include <pgm_base.h>
#include <reporter.h>
#include <settings/settings_manager.h>

#include <wx/filename.h>

#include <filesystem>
#include <fstream>
#include <iterator>


BOOST_AUTO_TEST_SUITE( JobsOpenBoard )


namespace
{

std::string readFile( const std::filesystem::path& aPath )
{
    std::ifstream in( aPath );
    return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
}

} // namespace


// The API server keeps a board open and edits it; a job on its file must see those edits, not a
// copy loaded from the file (which also stays cached after the first job)
BOOST_AUTO_TEST_CASE( BoardJobsUseTheOpenBoard )
{
    namespace fs = std::filesystem;

    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kicad_pcb_open_job" ) );

    for( const char* ext : { ".kicad_pcb", ".kicad_pro" } )
    {
        fs::copy_file( fs::path( KI_TEST::GetPcbnewTestDataDir() ) / ( std::string( "issue22536" ) + ext ),
                       dir.Path() / ( std::string( "issue22536" ) + ext ) );
    }

    wxString pcbPath = wxString::FromUTF8( ( dir.Path() / "issue22536.kicad_pcb" ).string().c_str() );

    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    mgr.LoadProject( wxString::FromUTF8( ( dir.Path() / "issue22536.kicad_pro" ).string().c_str() ) );
    PROJECT* project = &mgr.Prj();

    std::unique_ptr<BOARD> open = BOARD_LOADER::Load( pcbPath, PCB_IO_MGR::KICAD_SEXP, project );
    BOOST_REQUIRE( open );
    BOOST_REQUIRE( !open->Footprints().empty() );

    // An edit made after the file was saved
    open->Footprints().front()->SetValue( wxS( "API_EDIT" ) );

    PCBNEW_JOBS_HANDLER handler( nullptr );
    WX_STRING_REPORTER  reporter;
    handler.SetReporter( &reporter );

    auto exportPositions =
            [&]( const std::string& aName ) -> std::string
            {
                JOB_EXPORT_PCB_POS job;
                job.m_filename = pcbPath;
                job.m_format = JOB_EXPORT_PCB_POS::FORMAT::CSV;
                job.m_singleFile = true;
                job.m_nakedFilename = true;
                job.SetConfiguredOutputPath( wxString::FromUTF8( ( dir.Path() / aName ).string().c_str() ) );
                BOOST_REQUIRE_EQUAL( handler.JobExportPos( &job ), CLI::EXIT_CODES::OK );
                return readFile( dir.Path() / aName );
            };

    // With nothing open in the API server the job reads the file
    BOARD* none = nullptr;
    handler.SetOpenBoard( [&]() { return none; } );
    std::string fromFile = exportPositions( "file.csv" );
    BOOST_CHECK( fromFile.find( "Conn_01x04" ) != std::string::npos );
    BOOST_CHECK( fromFile.find( "API_EDIT" ) == std::string::npos );

    // With the board open, the job sees the unsaved edit
    BOARD* current = open.get();
    handler.SetOpenBoard( [&]() { return current; } );
    std::string fromOpen = exportPositions( "open.csv" );
    BOOST_CHECK( fromOpen.find( "API_EDIT" ) != std::string::npos );

    // A board replaced behind the provider (a revert) is the one used next
    std::unique_ptr<BOARD> reverted = BOARD_LOADER::Load( pcbPath, PCB_IO_MGR::KICAD_SEXP, project );
    BOOST_REQUIRE( reverted );
    current = reverted.get();
    std::string fromReverted = exportPositions( "reverted.csv" );
    BOOST_CHECK( fromReverted.find( "API_EDIT" ) == std::string::npos );
    BOOST_CHECK( fromReverted.find( "Conn_01x04" ) != std::string::npos );

    handler.SetOpenBoard( nullptr );
    handler.ClearCachedBoard();
    reverted.reset();
    open.reset();
    mgr.UnloadProject( project, false );
}


BOOST_AUTO_TEST_SUITE_END()
