/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <boost/test/unit_test.hpp>

#include <qa_utils/file_utils.h>
#include <qa_utils/wx_utils/unit_test_utils.h>

#include <cli/exit_codes.h>
#include <eeschema_helpers.h>
#include <eeschema_jobs_handler.h>
#include <jobs/job_export_bom.h>
#include <jobs/job_export_sch_netlist.h>
#include <jobs/job_export_sch_plot.h>
#include <jobs/job_export_sch_pythonbom.h>
#include <jobs/job_sch_erc.h>
#include <kiway.h>
#include <locale_io.h>
#include <pgm_base.h>
#include <reporter.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <settings/settings_manager.h>

#include <wx/filename.h>

#include <filesystem>
#include <fstream>
#include <iterator>


// A schematic PDF job whose output is a folder names the file after the schematic
BOOST_AUTO_TEST_CASE( SchPlotJobOutputFolderGetsDefaultName )
{
    LOCALE_IO dummy;

    namespace fs = std::filesystem;

    KI_TEST::SCOPED_TEMP_DIR outputDir( wxS( "kicad_sch_plot_folder" ) );

    fs::path schematic = fs::path( KI_TEST::GetTestDataRootDir() ) / "eeschema" / "issue24217" / "issue24217.kicad_sch";

    JOB_EXPORT_SCH_PLOT_PDF job;
    job.m_filename = wxString::FromUTF8( schematic.string().c_str() );

    // A path such as ${KIPRJMOD}/ resolves to a folder with a trailing separator
    job.SetConfiguredOutputPath( wxString::FromUTF8( outputDir.Path().string().c_str() )
                                 + wxFileName::GetPathSeparator() );

    EESCHEMA_JOBS_HANDLER handler( nullptr );
    WX_STRING_REPORTER    reporter;
    handler.SetReporter( &reporter );

    BOOST_CHECK_EQUAL( handler.JobExportPlot( &job ), CLI::EXIT_CODES::OK );
    BOOST_CHECK( fs::is_regular_file( outputDir.Path() / "issue24217.pdf" ) );
}


// ERC asks the kiway for CvPcb and runs without it
class NO_FACE_KIWAY : public KIWAY
{
public:
    NO_FACE_KIWAY() : KIWAY( KFCTL_STANDALONE ) {}

    KIFACE* KiFACE( FACE_T aFaceId, bool doLoad = true ) override { return nullptr; }
};


// Netlist, BOM and ERC jobs whose output is a folder also get their default file name
BOOST_AUTO_TEST_CASE( SchJobsOutputFolderGetDefaultName )
{
    LOCALE_IO dummy;

    namespace fs = std::filesystem;

    KI_TEST::SCOPED_TEMP_DIR outputDir( wxS( "kicad_sch_job_folder" ) );

    fs::path schematic = fs::path( KI_TEST::GetTestDataRootDir() ) / "eeschema" / "issue24217" / "issue24217.kicad_sch";
    wxString schPath = wxString::FromUTF8( schematic.string().c_str() );
    wxString folder = wxString::FromUTF8( outputDir.Path().string().c_str() ) + wxFileName::GetPathSeparator();

    JOB_EXPORT_SCH_NETLIST   netJob;
    JOB_EXPORT_BOM           bomJob;
    JOB_EXPORT_SCH_PYTHONBOM pyBomJob;
    JOB_SCH_ERC              ercJob;

    netJob.m_filename = schPath;
    bomJob.m_filename = schPath;
    pyBomJob.m_filename = schPath;
    ercJob.m_filename = schPath;
    ercJob.m_exitCodeViolations = false;

    NO_FACE_KIWAY         kiway;
    EESCHEMA_JOBS_HANDLER handler( &kiway );
    WX_STRING_REPORTER    reporter;
    handler.SetReporter( &reporter );

    using HANDLER_FN = int ( EESCHEMA_JOBS_HANDLER::* )( JOB* );

    std::vector<std::tuple<JOB*, HANDLER_FN, std::string>> cases = {
        { &netJob, &EESCHEMA_JOBS_HANDLER::JobExportNetlist, "issue24217.net" },
        { &bomJob, &EESCHEMA_JOBS_HANDLER::JobExportBom, "issue24217.csv" },
        { &pyBomJob, &EESCHEMA_JOBS_HANDLER::JobExportPythonBom, "issue24217-bom.xml" },
        { &ercJob, &EESCHEMA_JOBS_HANDLER::JobSchErc, "issue24217-erc.rpt" }
    };

    for( auto& [job, run, expected] : cases )
    {
        BOOST_TEST_CONTEXT( expected )
        {
            job->SetConfiguredOutputPath( folder );

            BOOST_CHECK_EQUAL( ( handler.*run )( job ), CLI::EXIT_CODES::OK );
            BOOST_CHECK_MESSAGE( fs::is_regular_file( outputDir.Path() / expected ), reporter.GetMessages() );
        }
    }
}


namespace
{

std::string readFile( const std::filesystem::path& aPath )
{
    std::ifstream in( aPath );
    return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
}

} // namespace


// The API server keeps a schematic open and edits it; a job on its file must see those edits,
// not a copy loaded from the file (which also stays cached after the first job)
BOOST_AUTO_TEST_CASE( SchJobsUseTheOpenSchematic )
{
    LOCALE_IO dummy;

    namespace fs = std::filesystem;

    KI_TEST::SCOPED_TEMP_DIR dir( wxS( "kicad_sch_open_job" ) );

    fs::path source = fs::path( KI_TEST::GetTestDataRootDir() ) / "eeschema" / "issue24217" / "issue24217.kicad_sch";
    fs::copy_file( source, dir.Path() / "issue24217.kicad_sch" );
    wxString schPath = wxString::FromUTF8( ( dir.Path() / "issue24217.kicad_sch" ).string().c_str() );

    SETTINGS_MANAGER& mgr = Pgm().GetSettingsManager();
    mgr.LoadProject( wxString::FromUTF8( ( dir.Path() / "issue24217.kicad_pro" ).string().c_str() ) );
    PROJECT* project = &mgr.Prj();

    SCHEMATIC* open = EESCHEMA_HELPERS::LoadSchematic( schPath, false, false, project );
    BOOST_REQUIRE( open );

    // An edit made after the file was saved
    SCH_SYMBOL* edited = nullptr;

    for( SCH_ITEM* item : open->RootScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetField( FIELD_T::VALUE )->GetText() == wxS( "6k2" ) )
            edited = symbol;
    }

    BOOST_REQUIRE( edited );
    edited->SetValueFieldText( wxS( "API_EDIT" ) );

    EESCHEMA_JOBS_HANDLER handler( nullptr );
    WX_STRING_REPORTER    reporter;
    handler.SetReporter( &reporter );

    auto exportNetlist =
            [&]( const std::string& aName ) -> std::string
            {
                JOB_EXPORT_SCH_NETLIST job;
                job.m_filename = schPath;
                job.format = JOB_EXPORT_SCH_NETLIST::FORMAT::KICADSEXPR;
                job.SetConfiguredOutputPath( wxString::FromUTF8( ( dir.Path() / aName ).string().c_str() ) );
                BOOST_REQUIRE_EQUAL( handler.JobExportNetlist( &job ), CLI::EXIT_CODES::OK );
                return readFile( dir.Path() / aName );
            };

    // With nothing open in the API server the job reads the file
    SCHEMATIC* none = nullptr;
    handler.SetOpenSchematic( &none );
    std::string fromFile = exportNetlist( "file.net" );
    BOOST_CHECK( fromFile.find( "6k2" ) != std::string::npos );

    // With the schematic open, the job sees the unsaved edit
    handler.SetOpenSchematic( &open );
    std::string fromOpen = exportNetlist( "open.net" );
    BOOST_CHECK( fromOpen.find( "API_EDIT" ) != std::string::npos );
    BOOST_CHECK( fromOpen.find( "\"6k2\"" ) == std::string::npos );

    // A schematic replaced in its slot (a revert) is the one used next
    SCHEMATIC* reverted = EESCHEMA_HELPERS::LoadSchematic( schPath, false, false, project );
    BOOST_REQUIRE( reverted );
    std::swap( open, reverted );
    BOOST_CHECK( exportNetlist( "reverted.net" ).find( "6k2" ) != std::string::npos );

    handler.SetOpenSchematic( nullptr );
    handler.ClearCachedSchematic();
    delete open;
    delete reverted;
    mgr.UnloadProject( project, false );
}
