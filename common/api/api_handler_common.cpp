/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
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

#include <cmath>
#include <optional>
#include <ranges>
#include <set>
#include <tuple>

#include <api/api_handler_common.h>
#include <wx/filename.h>
#include <api/api_task_scheduler.h>
#include <build_version.h>
#include <eda_shape.h>
#include <eda_text.h>
#include <gestfich.h>
#include <geometry/shape_compound.h>
#include <google/protobuf/empty.pb.h>
#include <paths.h>
#include <pgm_base.h>
#include <api/api_plugin.h>
#include <api/api_enums.h>
#include <api/api_utils.h>
#include <project/net_settings.h>
#include <project/project_file.h>
#include <project/tuning_profiles.h>
#include <settings/settings_manager.h>
#include <transline_calculations/coplanar.h>
#include <transline_calculations/coupled_microstrip.h>
#include <transline_calculations/coupled_stripline.h>
#include <transline_calculations/microstrip.h>
#include <transline_calculations/stripline.h>
#include <transline_calculations/units.h>
#include <wx/string.h>

using namespace kiapi::common::commands;
using namespace kiapi::common::types;
using google::protobuf::Empty;


API_HANDLER_COMMON::API_HANDLER_COMMON() :
        API_HANDLER()
{
    registerHandler<commands::GetVersion, GetVersionResponse>( &API_HANDLER_COMMON::handleGetVersion );
    registerHandler<GetKiCadBinaryPath, PathResponse>(
            &API_HANDLER_COMMON::handleGetKiCadBinaryPath );
    registerHandler<GetPaths, GetPathsResponse>( &API_HANDLER_COMMON::handleGetPaths );
    registerHandler<GetNetClasses, NetClassesResponse>( &API_HANDLER_COMMON::handleGetNetClasses );
    registerHandler<SetNetClasses, Empty>( &API_HANDLER_COMMON::handleSetNetClasses );
    registerHandler<GetNetClassAssignments, NetClassAssignmentsResponse>(
            &API_HANDLER_COMMON::handleGetNetClassAssignments );
    registerHandler<SetNetClassAssignments, Empty>( &API_HANDLER_COMMON::handleSetNetClassAssignments );
    registerHandler<GetTuningProfiles, TuningProfilesResponse>( &API_HANDLER_COMMON::handleGetTuningProfiles );
    registerHandler<SetTuningProfiles, Empty>( &API_HANDLER_COMMON::handleSetTuningProfiles );
    registerHandler<Ping, Empty>( &API_HANDLER_COMMON::handlePing );
    registerHandler<GetTextExtents, types::Box2>( &API_HANDLER_COMMON::handleGetTextExtents );
    registerHandler<GetTextAsShapes, GetTextAsShapesResponse>(
            &API_HANDLER_COMMON::handleGetTextAsShapes );
    registerHandler<ExpandTextVariables, ExpandTextVariablesResponse>(
            &API_HANDLER_COMMON::handleExpandTextVariables );
    registerHandler<GetPluginSettingsPath, StringResponse>(
            &API_HANDLER_COMMON::handleGetPluginSettingsPath );
    registerHandler<GetTextVariables, project::TextVariables>(
            &API_HANDLER_COMMON::handleGetTextVariables );
    registerHandler<SetTextVariables, Empty>(
            &API_HANDLER_COMMON::handleSetTextVariables );
    registerHandler<OpenDocument, OpenDocumentResponse>(
            &API_HANDLER_COMMON::handleOpenDocument );
    registerHandler<CloseDocument, Empty>(
            &API_HANDLER_COMMON::handleCloseDocument );
    registerHandler<CloseAllDocuments, Empty>(
            &API_HANDLER_COMMON::handleCloseAllDocuments );
    registerHandler<CreateDocument, OpenDocumentResponse>(
            &API_HANDLER_COMMON::handleCreateDocument );
    registerHandler<tasks::GetTaskStatus, tasks::TaskStatusResponse>(
            &API_HANDLER_COMMON::handleGetTaskStatus, true );
    registerHandler<CalculateTransmissionLine, TransmissionLineResponse>(
            &API_HANDLER_COMMON::handleCalculateTransmissionLine );
}


HANDLER_RESULT<GetVersionResponse> API_HANDLER_COMMON::handleGetVersion(
        const HANDLER_CONTEXT<commands::GetVersion>& )
{
    GetVersionResponse reply;

    reply.mutable_version()->set_full_version( GetBuildVersion().ToStdString() );

    std::tuple<int, int, int> version = GetMajorMinorPatchTuple();
    reply.mutable_version()->set_major( std::get<0>( version ) );
    reply.mutable_version()->set_minor( std::get<1>( version ) );
    reply.mutable_version()->set_patch( std::get<2>( version ) );

    return reply;
}


HANDLER_RESULT<PathResponse> API_HANDLER_COMMON::handleGetKiCadBinaryPath(
        const HANDLER_CONTEXT<GetKiCadBinaryPath>& aCtx )
{
    wxFileName fn( wxEmptyString, wxString::FromUTF8( aCtx.Request.binary_name() ) );
#ifdef _WIN32
    fn.SetExt( wxT( "exe" ) );
#endif

    wxString path = FindKicadFile( fn.GetFullName() );
    PathResponse reply;
    reply.set_path( path.ToUTF8() );
    return reply;
}


tl::expected<bool, ApiResponseStatus> API_HANDLER_COMMON::validateProject( const ProjectSpecifier& aProject,
                                                                           bool aAllowEmpty )
{
    if( !aAllowEmpty && ( aProject.name().empty() || aProject.path().empty() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "a project name and path must be specified" );
        return tl::unexpected( e );
    }

    const PROJECT& prj = Pgm().GetSettingsManager().Prj();

    if( !aProject.name().empty() && aProject.name() != prj.GetProjectName().utf8_string() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested project {} is not open", aProject.name() ) );
        return tl::unexpected( e );
    }

    // Document specifiers from the editors carry the project directory without a trailing
    // separator while GetProjectPath() has one, so compare the directories, not the strings.
    auto sameDirectory =
            []( const wxString& aLeft, const wxString& aRight )
            {
                return wxFileName::DirName( aLeft ).GetPath() == wxFileName::DirName( aRight ).GetPath();
            };

    if( !aProject.path().empty()
        && !sameDirectory( wxString::FromUTF8( aProject.path() ), prj.GetProjectPath() ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "the requested project {} is not open at path {}",
                                          aProject.name(),
                                          aProject.path() ) );
        return tl::unexpected( e );
    }

    return true;
}


HANDLER_RESULT<NetClassesResponse> API_HANDLER_COMMON::handleGetNetClasses( const HANDLER_CONTEXT<GetNetClasses>& aCtx )
{
    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.project(), true ); !result )
        return tl::unexpected( result.error() );

    NetClassesResponse reply;

    std::shared_ptr<NET_SETTINGS>& netSettings = Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    google::protobuf::Any any;

    netSettings->GetDefaultNetclass()->Serialize( any );

    if( !any.UnpackTo( reply.add_net_classes() ) )
        wxFAIL_MSG( wxT( "Failed to unpack Default netclasss from internal serialization" ) );

    for( const std::shared_ptr<NETCLASS>& netClass : netSettings->GetNetclasses() | std::views::values )
    {
        netClass->Serialize( any );

        if( !any.UnpackTo( reply.add_net_classes() ) )
        {
            wxFAIL_MSG( wxString::Format( wxT( "Failed to unpack netclass %s from internal serialization" ),
                                          netClass->GetName() ) );
        }
    }

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleSetNetClasses( const HANDLER_CONTEXT<SetNetClasses>& aCtx )
{
    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.project(), true ); !result )
        return tl::unexpected( result.error() );

    std::shared_ptr<NET_SETTINGS>& netSettings = Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    std::map<wxString, std::shared_ptr<NETCLASS>> netClasses;
    google::protobuf::Any                         any;

    if( aCtx.Request.merge_mode() != MapMergeMode::MMM_REPLACE )
        netClasses = netSettings->GetNetclasses();

    for( const kiapi::common::project::NetClass& ncProto : aCtx.Request.net_classes() )
    {
        wxString name = wxString::FromUTF8( ncProto.name() );
        bool deserialized = false;

        if( any.PackFrom( ncProto ) )
        {
            if( name == wxT( "Default" ) )
            {
                deserialized = netSettings->GetDefaultNetclass()->Deserialize( any );
            }
            else
            {
                if( !netClasses.contains( name ) )
                    netClasses.insert( { name, std::make_shared<NETCLASS>( name, false ) } );

                deserialized = netClasses[name]->Deserialize( any );
            }
        }

        if( !deserialized )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( fmt::format( "could not unpack netclass '{}'", name.ToUTF8().data() ) );
            return tl::unexpected( e );
        }
    }

    netSettings->SetNetclasses( netClasses );
    requestNetSettingsNotification();

    return Empty();
}


HANDLER_RESULT<TuningProfilesResponse>
API_HANDLER_COMMON::handleGetTuningProfiles( const HANDLER_CONTEXT<GetTuningProfiles>& aCtx )
{
    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.project(), true ); !result )
        return tl::unexpected( result.error() );

    using kiapi::board::types::BoardLayer;

    TuningProfilesResponse reply;

    std::shared_ptr<TUNING_PROFILES>& profiles =
            Pgm().GetSettingsManager().Prj().GetProjectFile().TuningProfileParameters();

    if( !profiles )
        return reply;

    for( const TUNING_PROFILE& profile : profiles->GetTuningProfiles() )
    {
        project::TuningProfile* out = reply.add_profiles();
        out->set_name( profile.m_ProfileName.ToUTF8() );
        out->set_type( profile.m_Type == TUNING_PROFILE::PROFILE_TYPE::DIFFERENTIAL
                               ? project::TuningProfileType::TPT_DIFFERENTIAL
                               : project::TuningProfileType::TPT_SINGLE );
        out->set_target_impedance( profile.m_TargetImpedance );
        out->set_frequency( profile.m_Frequency );
        out->set_enable_time_domain_tuning( profile.m_EnableTimeDomainTuning );
        out->set_model_solder_mask( profile.m_ModelSolderMask );
        out->set_via_delay_per_mm( profile.m_ViaPropagationDelay );
        out->set_net_chain_bridge_delay_per_mm( profile.m_NetChainBridgePropagationDelay );

        for( const DELAY_PROFILE_TRACK_PROPAGATION_ENTRY& entry : profile.m_TrackPropagationEntries )
        {
            project::TuningProfileLayer* layer = out->add_layers();
            layer->set_signal_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.GetSignalLayer() ) );
            layer->set_top_reference_layer( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.GetTopReferenceLayer() ) );
            layer->set_bottom_reference_layer(
                    ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.GetBottomReferenceLayer() ) );
            layer->mutable_width()->set_value_nm( entry.GetWidth() );
            layer->mutable_diff_pair_gap()->set_value_nm( entry.GetDiffPairGap() );
            layer->set_delay_per_mm( entry.GetDelay( true ) );
        }

        for( const DELAY_PROFILE_VIA_OVERRIDE_ENTRY& entry : profile.m_ViaOverrides )
        {
            project::TuningProfileViaOverride* via = out->add_via_overrides();
            via->set_signal_layer_from( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.m_SignalLayerFrom ) );
            via->set_signal_layer_to( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.m_SignalLayerTo ) );
            via->set_via_layer_from( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.m_ViaLayerFrom ) );
            via->set_via_layer_to( ToProtoEnum<PCB_LAYER_ID, BoardLayer>( entry.m_ViaLayerTo ) );
            via->mutable_delay()->set_value_as( entry.m_Delay );
        }
    }

    return reply;
}


HANDLER_RESULT<Empty>
API_HANDLER_COMMON::handleSetTuningProfiles( const HANDLER_CONTEXT<SetTuningProfiles>& aCtx )
{
    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.project(), true ); !result )
        return tl::unexpected( result.error() );

    auto badRequest =
            []( const std::string& aMessage ) -> HANDLER_RESULT<Empty>
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( aMessage );
                return tl::unexpected( e );
            };

    std::shared_ptr<TUNING_PROFILES>& profiles =
            Pgm().GetSettingsManager().Prj().GetProjectFile().TuningProfileParameters();

    if( !profiles )
        return badRequest( "the project has no tuning profile settings" );

    // Delays and dimensions are stored as int
    constexpr int64_t intMax = std::numeric_limits<int>::max();

    auto copperLayer =
            []( kiapi::board::types::BoardLayer aLayer, bool aAllowNone ) -> std::optional<PCB_LAYER_ID>
            {
                PCB_LAYER_ID layer = FromProtoEnum<PCB_LAYER_ID>( aLayer );

                if( aAllowNone && layer == UNDEFINED_LAYER )
                    return layer;

                if( !IsCopperLayer( layer ) )
                    return std::nullopt;

                return layer;
            };

    std::vector<TUNING_PROFILE> incoming;
    std::set<wxString>          names;

    for( const project::TuningProfile& in : aCtx.Request.profiles() )
    {
        wxString name = wxString::FromUTF8( in.name() );
        name.Trim( true ).Trim( false );

        if( name.IsEmpty() )
            return badRequest( "a tuning profile must have a name" );

        if( !names.insert( name ).second )
            return badRequest( fmt::format( "tuning profile '{}' is given more than once", in.name() ) );

        TUNING_PROFILE profile{};
        profile.m_ProfileName = name;

        switch( in.type() )
        {
        case project::TuningProfileType::TPT_SINGLE:
            profile.m_Type = TUNING_PROFILE::PROFILE_TYPE::SINGLE;
            break;

        case project::TuningProfileType::TPT_DIFFERENTIAL:
            profile.m_Type = TUNING_PROFILE::PROFILE_TYPE::DIFFERENTIAL;
            break;

        default:
            return badRequest( fmt::format( "tuning profile '{}' needs a type", in.name() ) );
        }

        if( !std::isfinite( in.target_impedance() ) || !std::isfinite( in.frequency() )
            || in.target_impedance() < 0 || in.frequency() < 0 || in.via_delay_per_mm() < 0
            || in.via_delay_per_mm() > intMax || in.net_chain_bridge_delay_per_mm() < 0
            || in.net_chain_bridge_delay_per_mm() > intMax )
        {
            return badRequest( fmt::format( "tuning profile '{}' has a negative or out of range value",
                                            in.name() ) );
        }

        profile.m_TargetImpedance = in.target_impedance();
        profile.m_Frequency = in.frequency() > 0 ? in.frequency() : 1e9;
        profile.m_EnableTimeDomainTuning = in.enable_time_domain_tuning();
        profile.m_ModelSolderMask = in.model_solder_mask();
        profile.m_ViaPropagationDelay = static_cast<int>( in.via_delay_per_mm() );
        profile.m_NetChainBridgePropagationDelay = static_cast<int>( in.net_chain_bridge_delay_per_mm() );

        for( const project::TuningProfileLayer& layerIn : in.layers() )
        {
            std::optional<PCB_LAYER_ID> signal = copperLayer( layerIn.signal_layer(), false );
            std::optional<PCB_LAYER_ID> top = copperLayer( layerIn.top_reference_layer(), true );
            std::optional<PCB_LAYER_ID> bottom = copperLayer( layerIn.bottom_reference_layer(), true );

            if( !signal || !top || !bottom )
            {
                return badRequest( fmt::format( "tuning profile '{}': signal and reference layers must be "
                                                "copper layers", in.name() ) );
            }

            if( profile.m_TrackPropagationEntriesMap.contains( *signal ) )
            {
                return badRequest( fmt::format( "tuning profile '{}': signal layer {} is given more than once",
                                                in.name(), LSET::Name( *signal ).ToStdString() ) );
            }

            if( layerIn.width().value_nm() < 0 || layerIn.width().value_nm() > intMax
                || layerIn.diff_pair_gap().value_nm() < 0 || layerIn.diff_pair_gap().value_nm() > intMax
                || layerIn.delay_per_mm() < 0 || layerIn.delay_per_mm() > intMax )
            {
                return badRequest( fmt::format( "tuning profile '{}': layer {} has a negative or out of "
                                                "range value", in.name(), LSET::Name( *signal ).ToStdString() ) );
            }

            DELAY_PROFILE_TRACK_PROPAGATION_ENTRY entry;
            entry.SetSignalLayer( *signal );
            entry.SetTopReferenceLayer( *top );
            entry.SetBottomReferenceLayer( *bottom );
            entry.SetWidth( static_cast<int>( layerIn.width().value_nm() ) );
            entry.SetDiffPairGap( static_cast<int>( layerIn.diff_pair_gap().value_nm() ) );
            entry.SetDelay( static_cast<int>( layerIn.delay_per_mm() ) );
            entry.SetEnableTimeDomainTuning( profile.m_EnableTimeDomainTuning );

            profile.m_TrackPropagationEntries.push_back( entry );

            // The delay lookup goes through this map, so it must be filled as when loading a project
            profile.m_TrackPropagationEntriesMap[*signal] = entry;
        }

        for( const project::TuningProfileViaOverride& viaIn : in.via_overrides() )
        {
            std::optional<PCB_LAYER_ID> signalFrom = copperLayer( viaIn.signal_layer_from(), false );
            std::optional<PCB_LAYER_ID> signalTo = copperLayer( viaIn.signal_layer_to(), false );
            std::optional<PCB_LAYER_ID> viaFrom = copperLayer( viaIn.via_layer_from(), false );
            std::optional<PCB_LAYER_ID> viaTo = copperLayer( viaIn.via_layer_to(), false );

            if( !signalFrom || !signalTo || !viaFrom || !viaTo )
            {
                return badRequest( fmt::format( "tuning profile '{}': via override layers must be copper "
                                                "layers", in.name() ) );
            }

            if( viaIn.delay().value_as() < 0 || viaIn.delay().value_as() > intMax )
            {
                return badRequest( fmt::format( "tuning profile '{}': via override delay is negative or out "
                                                "of range", in.name() ) );
            }

            // The delay lookup expects the upper layer first, as the board setup dialog saves them
            if( IsCopperLayerLowerThan( *signalFrom, *signalTo ) )
                std::swap( signalFrom, signalTo );

            if( IsCopperLayerLowerThan( *viaFrom, *viaTo ) )
                std::swap( viaFrom, viaTo );

            profile.m_ViaOverrides.push_back( DELAY_PROFILE_VIA_OVERRIDE_ENTRY{
                    *signalFrom, *signalTo, *viaFrom, *viaTo, static_cast<int>( viaIn.delay().value_as() ) } );
        }

        std::sort( profile.m_ViaOverrides.begin(), profile.m_ViaOverrides.end() );
        incoming.push_back( std::move( profile ) );
    }

    std::vector<TUNING_PROFILE> result;

    if( aCtx.Request.merge_mode() != MapMergeMode::MMM_REPLACE )
    {
        for( const TUNING_PROFILE& existing : profiles->GetTuningProfiles() )
        {
            if( !names.contains( existing.m_ProfileName ) )
                result.push_back( existing );
        }
    }

    for( TUNING_PROFILE& profile : incoming )
        result.push_back( std::move( profile ) );

    profiles->ClearTuningProfiles();

    for( TUNING_PROFILE& profile : result )
        profiles->AddTuningProfile( std::move( profile ) );

    // Boards cache pointers into the profile list; the notification rebuilds those caches
    requestNetSettingsNotification();

    return Empty();
}


HANDLER_RESULT<NetClassAssignmentsResponse>
API_HANDLER_COMMON::handleGetNetClassAssignments( const HANDLER_CONTEXT<GetNetClassAssignments>& aCtx )
{
    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.project() ); !result )
        return tl::unexpected( result.error() );

    std::shared_ptr<NET_SETTINGS>& netSettings = Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    NetClassAssignmentsResponse reply;

    for( const auto& [netName, netclassNames] : netSettings->GetNetclassLabelAssignments() )
    {
        project::NetClassAssignment* assignment = reply.add_assignments();
        assignment->set_net( netName.ToUTF8() );

        for( const wxString& netclassName : netclassNames )
            assignment->add_netclasses( netclassName.ToUTF8() );
    }

    for( const auto& [matcher, netclassName] : netSettings->GetNetclassPatternAssignments() )
    {
        project::NetClassPatternAssignment* pattern = reply.add_pattern_assignments();
        pattern->set_pattern( matcher->GetPattern().ToUTF8() );
        pattern->set_netclass( netclassName.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<Empty>
API_HANDLER_COMMON::handleSetNetClassAssignments( const HANDLER_CONTEXT<SetNetClassAssignments>& aCtx )
{
    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.project() ); !result )
        return tl::unexpected( result.error() );

    std::shared_ptr<NET_SETTINGS>& netSettings = Pgm().GetSettingsManager().Prj().GetProjectFile().m_NetSettings;

    std::set<wxString, std::less<>> knownNetclasses;
    knownNetclasses.insert( NETCLASS::Default );

    for( const wxString& name : netSettings->GetNetclasses() | std::views::keys )
        knownNetclasses.insert( name );

    auto checkNetclassName =
            [&]( const std::string& aName, const char* aKind ) -> std::optional<ApiResponseStatus>
            {
                if( knownNetclasses.contains( wxString::FromUTF8( aName ) ) )
                    return std::nullopt;

                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( fmt::format( "unknown netclass '{}' in {} assignment", aName, aKind ) );
                return e;
            };

    for( const project::NetClassAssignment& assignment : aCtx.Request.assignments() )
    {
        if( assignment.net().empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "net name cannot be empty in a netclass assignment" );
            return tl::unexpected( e );
        }

        for( const std::string& netclassName : assignment.netclasses() )
        {
            if( std::optional<ApiResponseStatus> err = checkNetclassName( netclassName, "net" ) )
                return tl::unexpected( *err );
        }
    }

    for( const project::NetClassPatternAssignment& pattern : aCtx.Request.pattern_assignments() )
    {
        if( pattern.pattern().empty() )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "pattern cannot be empty in a netclass pattern assignment" );
            return tl::unexpected( e );
        }

        if( !pattern.netclass().empty() )
        {
            if( std::optional<ApiResponseStatus> err = checkNetclassName( pattern.netclass(), "pattern" ) )
                return tl::unexpected( *err );
        }
    }

    if( aCtx.Request.merge_mode() == MapMergeMode::MMM_REPLACE )
    {
        netSettings->ClearNetclassLabelAssignments();
        netSettings->ClearNetclassPatternAssignments();
    }

    for( const project::NetClassAssignment& assignment : aCtx.Request.assignments() )
    {
        std::set<wxString> netclasses;

        for( const std::string& netclassName : assignment.netclasses() )
            netclasses.insert( wxString::FromUTF8( netclassName ) );

        if( netclasses.empty() )
            netSettings->ClearNetclassLabelAssignment( wxString::FromUTF8( assignment.net() ) );
        else
            netSettings->SetNetclassLabelAssignment( wxString::FromUTF8( assignment.net() ), netclasses );
    }

    for( const project::NetClassPatternAssignment& pattern : aCtx.Request.pattern_assignments() )
    {
        std::vector<std::pair<std::unique_ptr<EDA_COMBINED_MATCHER>, wxString>> kept;

        for( auto& existing : netSettings->GetNetclassPatternAssignments() )
        {
            if( existing.first->GetPattern() != wxString::FromUTF8( pattern.pattern() ) )
                kept.emplace_back( std::move( existing ) );
        }

        netSettings->SetNetclassPatternAssignments( std::move( kept ) );

        if( !pattern.netclass().empty() )
        {
            netSettings->SetNetclassPatternAssignment( wxString::FromUTF8( pattern.pattern() ),
                                                       wxString::FromUTF8( pattern.netclass() ) );
        }
    }

    netSettings->ClearAllCaches();
    requestNetSettingsNotification();

    return Empty();
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handlePing( const HANDLER_CONTEXT<Ping>& aCtx )
{
    return Empty();
}


HANDLER_RESULT<types::Box2> API_HANDLER_COMMON::handleGetTextExtents( const HANDLER_CONTEXT<GetTextExtents>& aCtx )
{
    EDA_TEXT text( pcbIUScale );
    google::protobuf::Any any;

    if( !any.PackFrom( aCtx.Request.text() ) || !text.Deserialize( any ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "Could not decode text in GetTextExtents message" );
        return tl::unexpected( e );
    }

    types::Box2 response;

    BOX2I bbox = text.GetTextBox( nullptr );
    EDA_ANGLE angle = text.GetTextAngle();

    if( !angle.IsZero() )
        bbox = bbox.GetBoundingBoxRotated( text.GetTextPos(), text.GetTextAngle() );

    response.mutable_position()->set_x_nm( bbox.GetPosition().x );
    response.mutable_position()->set_y_nm( bbox.GetPosition().y );
    response.mutable_size()->set_x_nm( bbox.GetSize().x );
    response.mutable_size()->set_y_nm( bbox.GetSize().y );

    return response;
}


HANDLER_RESULT<GetTextAsShapesResponse> API_HANDLER_COMMON::handleGetTextAsShapes(
        const HANDLER_CONTEXT<GetTextAsShapes>& aCtx )
{
    GetTextAsShapesResponse reply;

    for( const TextOrTextBox& textMsg : aCtx.Request.text() )
    {
        Text dummyText;
        const Text* textPtr = &textMsg.text();

        if( textMsg.has_textbox() )
        {
            dummyText.set_text( textMsg.textbox().text() );
            dummyText.mutable_attributes()->CopyFrom( textMsg.textbox().attributes() );
            textPtr = &dummyText;
        }

        EDA_TEXT text( pcbIUScale );
        google::protobuf::Any any;

        if( !any.PackFrom( *textPtr ) || !text.Deserialize( any ) )
        {
            ApiResponseStatus e;
            e.set_status( ApiStatusCode::AS_BAD_REQUEST );
            e.set_error_message( "Could not decode text in GetTextAsShapes message" );
            return tl::unexpected( e );
        }

        std::shared_ptr<SHAPE_COMPOUND> shapes = text.GetEffectiveTextShape( false );

        TextWithShapes* entry = reply.add_text_with_shapes();
        entry->mutable_text()->CopyFrom( textMsg );

        for( SHAPE* subshape : shapes->Shapes() )
        {
            EDA_SHAPE proxy( *subshape );
            proxy.Serialize( any );
            GraphicShape* shapeMsg = entry->mutable_shapes()->add_shapes();

            if( !any.UnpackTo( shapeMsg ) )
                wxFAIL_MSG( wxT( "Failed to unpack text-as-shapes component from internal serialization" ) );
        }

        if( textMsg.has_textbox() && textMsg.textbox().border_enabled() )
        {
            GraphicShape* border = entry->mutable_shapes()->add_shapes();
            int width = (int) textMsg.textbox().attributes().stroke_width().value_nm();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            VECTOR2I tl = UnpackVector2( textMsg.textbox().top_left() );
            VECTOR2I br = UnpackVector2( textMsg.textbox().bottom_right() );

            // top
            PackVector2( *border->mutable_segment()->mutable_start(), tl );
            PackVector2( *border->mutable_segment()->mutable_end(), VECTOR2I( br.x, tl.y ) );

            // right
            border = entry->mutable_shapes()->add_shapes();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), VECTOR2I( br.x, tl.y ) );
            PackVector2( *border->mutable_segment()->mutable_end(), br );

            // bottom
            border = entry->mutable_shapes()->add_shapes();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), br );
            PackVector2( *border->mutable_segment()->mutable_end(), VECTOR2I( tl.x, br.y ) );

            // left
            border = entry->mutable_shapes()->add_shapes();
            border->mutable_attributes()->mutable_stroke()->mutable_width()->set_value_nm( width );
            PackVector2( *border->mutable_segment()->mutable_start(), VECTOR2I( tl.x, br.y ) );
            PackVector2( *border->mutable_segment()->mutable_end(), tl );
        }
    }

    return reply;
}


HANDLER_RESULT<ExpandTextVariablesResponse> API_HANDLER_COMMON::handleExpandTextVariables(
        const HANDLER_CONTEXT<ExpandTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    ExpandTextVariablesResponse reply;
    PROJECT& project = Pgm().GetSettingsManager().Prj();

    for( const std::string& textMsg : aCtx.Request.text() )
    {
        wxString result = ExpandTextVars( wxString::FromUTF8( textMsg ), &project, INTERNAL );

        if( aCtx.Request.expand_env_vars() )
            result = ExpandEnvVarSubstitutions( result, &project );

        reply.add_text( result.ToUTF8() );
    }

    return reply;
}


HANDLER_RESULT<StringResponse> API_HANDLER_COMMON::handleGetPluginSettingsPath(
        const HANDLER_CONTEXT<GetPluginSettingsPath>& aCtx )
{
    wxString identifier = wxString::FromUTF8( aCtx.Request.identifier() );

    if( identifier.IsEmpty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "plugin identifier is missing" );
        return tl::unexpected( e );
    }

    if( !API_PLUGIN::IsValidIdentifier( identifier ) )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "plugin identifier is invalid" );
        return tl::unexpected( e );
    }

    wxFileName path( PATHS::GetUserSettingsPath(), wxEmptyString );
    path.AppendDir( "plugins" );

    // Create the base plugins path if needed, but leave the specific plugin to create its own path
    PATHS::EnsurePathExists( path.GetPath() );

    path.AppendDir( identifier );

    StringResponse reply;
    reply.set_response( path.GetPath() );
    return reply;
}


HANDLER_RESULT<project::TextVariables> API_HANDLER_COMMON::handleGetTextVariables(
        const HANDLER_CONTEXT<GetTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.document().project() );
        !result )
    {
        return tl::unexpected( result.error() );
    }

    const PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "no valid project is loaded, cannot get text variables" );
        return tl::unexpected( e );
    }

    const std::map<wxString, wxString>& vars = project.GetTextVars();

    project::TextVariables reply;
    auto map = reply.mutable_variables();

    for( const auto& [key, value] : vars )
        ( *map )[ std::string( key.ToUTF8() ) ] = value.ToUTF8();

    return reply;
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleSetTextVariables(
    const HANDLER_CONTEXT<SetTextVariables>& aCtx )
{
    if( !aCtx.Request.has_document() || aCtx.Request.document().type() != DOCTYPE_PROJECT )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNHANDLED );
        // No error message, this is a flag that the server should try a different handler
        return tl::unexpected( e );
    }

    if( tl::expected<bool, ApiResponseStatus> result = validateProject( aCtx.Request.document().project() );
        !result )
    {
        return tl::unexpected( result.error() );
    }

    PROJECT& project = Pgm().GetSettingsManager().Prj();

    if( project.IsNullProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_NOT_READY );
        e.set_error_message( "no valid project is loaded, cannot set text variables" );
        return tl::unexpected( e );
    }

    const project::TextVariables& newVars = aCtx.Request.variables();
    std::map<wxString, wxString>& vars = project.GetTextVars();

    if( aCtx.Request.merge_mode() == MapMergeMode::MMM_REPLACE )
        vars.clear();

    for( const auto& [key, value] : newVars.variables() )
        vars[wxString::FromUTF8( key )] = wxString::FromUTF8( value );

    if( !Pgm().GetSettingsManager().SaveProject() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_INTERNAL_ERROR );
        e.set_error_message( "failed to save project text variables" );
        return tl::unexpected( e );
    }

    return Empty();
}


HANDLER_RESULT<OpenDocumentResponse> API_HANDLER_COMMON::handleOpenDocument(
        const HANDLER_CONTEXT<OpenDocument>& aCtx )
{
    if( !m_openDocumentHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "OpenDocument is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_openDocumentHandler( aCtx.Request );
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleCloseDocument(
        const HANDLER_CONTEXT<CloseDocument>& aCtx )
{
    if( !m_closeDocumentHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "CloseDocument is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_closeDocumentHandler( aCtx.Request );
}


HANDLER_RESULT<OpenDocumentResponse>
API_HANDLER_COMMON::handleCreateDocument( const HANDLER_CONTEXT<CreateDocument>& aCtx )
{
    if( !m_createDocumentHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "CreateDocument is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_createDocumentHandler( aCtx.Request );
}


HANDLER_RESULT<Empty> API_HANDLER_COMMON::handleCloseAllDocuments( const HANDLER_CONTEXT<CloseAllDocuments>& aCtx )
{
    if( !m_closeAllDocumentsHandler )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_UNIMPLEMENTED );
        e.set_error_message( "CloseAllDocuments is not available in this KiCad mode" );
        return tl::unexpected( e );
    }

    return m_closeAllDocumentsHandler( aCtx.Request );
}


HANDLER_RESULT<GetPathsResponse> API_HANDLER_COMMON::handleGetPaths( const HANDLER_CONTEXT<GetPaths>& )
{
    GetPathsResponse reply;

    auto addPath = [&]( types::PathType aType, const wxString& aPath )
    {
        PathEntry* entry = reply.add_paths();
        entry->set_type( aType );
        entry->set_path( aPath.ToUTF8() );
    };

    addPath( types::PATH_USER_PLUGINS, PATHS::GetUserPluginsPath() );
    addPath( types::PATH_USER_TEMPLATES, PATHS::GetUserTemplatesPath() );
    addPath( types::PATH_USER_SETTINGS, PATHS::GetUserSettingsPath() );
    addPath( types::PATH_STOCK_SYMBOLS, PATHS::GetStockSymbolsPath() );
    addPath( types::PATH_STOCK_FOOTPRINTS, PATHS::GetStockFootprintsPath() );
    addPath( types::PATH_STOCK_DESIGN_BLOCKS, PATHS::GetStockDesignBlocksPath() );
    addPath( types::PATH_STOCK_3DMODELS, PATHS::GetStock3dmodelsPath() );
    addPath( types::PATH_STOCK_TEMPLATES, PATHS::GetStockTemplatesPath() );

    return reply;
}


HANDLER_RESULT<tasks::TaskStatusResponse> API_HANDLER_COMMON::handleGetTaskStatus(
        const HANDLER_CONTEXT<tasks::GetTaskStatus>& aCtx )
{
    if( !aCtx.Request.has_token() || aCtx.Request.token().value().empty() )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( "no task token provided" );
        return tl::unexpected( e );
    }

    std::optional<tasks::TaskStatusResponse> status =
            API_TASK_SCHEDULER::Instance().GetStatus( aCtx.Request.token().value() );

    if( !status )
    {
        ApiResponseStatus e;
        e.set_status( ApiStatusCode::AS_BAD_REQUEST );
        e.set_error_message( fmt::format( "unknown task token {}", aCtx.Request.token().value() ) );
        return tl::unexpected( e );
    }

    return *status;
}


namespace
{

// The proto parameter enum mirrors TRANSLINE_PARAMETERS shifted by one (proto zero is UNKNOWN_ID)
TRANSLINE_PARAMETERS translineParamFromProto( TransmissionLineParameter aParam )
{
    int value = static_cast<int>( aParam ) - 1;

    if( value < 0 || value >= static_cast<int>( TRANSLINE_PARAMETERS::EXTRAS_COUNT ) )
        return TRANSLINE_PARAMETERS::UNKNOWN_ID;

    return static_cast<TRANSLINE_PARAMETERS>( value );
}


TransmissionLineParameter translineParamToProto( TRANSLINE_PARAMETERS aParam )
{
    return static_cast<TransmissionLineParameter>( static_cast<int>( aParam ) + 1 );
}


bool translineHasParam( const TRANSLINE_CALCULATION_BASE& aCalc, TRANSLINE_PARAMETERS aParam )
{
    try
    {
        aCalc.GetParameter( aParam );
        return true;
    }
    catch( const std::out_of_range& )
    {
        return false;
    }
}

} // namespace


HANDLER_RESULT<TransmissionLineResponse> API_HANDLER_COMMON::handleCalculateTransmissionLine(
        const HANDLER_CONTEXT<CalculateTransmissionLine>& aCtx )
{
    using TCP = TRANSLINE_PARAMETERS;

    auto badRequest =
            []( const std::string& aMessage ) -> HANDLER_RESULT<TransmissionLineResponse>
            {
                ApiResponseStatus e;
                e.set_status( ApiStatusCode::AS_BAD_REQUEST );
                e.set_error_message( aMessage );
                return tl::unexpected( e );
            };

    const CalculateTransmissionLine& request = aCtx.Request;
    std::unique_ptr<TRANSLINE_CALCULATION_BASE> calc;

    switch( request.model() )
    {
    case TLM_MICROSTRIP:         calc = std::make_unique<MICROSTRIP>();         break;
    case TLM_COUPLED_MICROSTRIP: calc = std::make_unique<COUPLED_MICROSTRIP>(); break;
    case TLM_STRIPLINE:          calc = std::make_unique<STRIPLINE>();          break;
    case TLM_COUPLED_STRIPLINE:  calc = std::make_unique<COUPLED_STRIPLINE>();  break;
    case TLM_COPLANAR:           calc = std::make_unique<COPLANAR>();           break;
    default:                     return badRequest( "an unknown transmission line model was given" );
    }

    // Copper resistivity used by the PCB Calculator and the tuning profile calculations
    constexpr double copperRho = 1.72e-8;

    std::map<TCP, double> given;
    std::optional<double> rho;

    for( const TransmissionLineValue& value : request.parameters() )
    {
        TCP param = translineParamFromProto( value.parameter() );

        if( param == TCP::UNKNOWN_ID )
            return badRequest( "an unknown transmission line parameter was given" );

        if( !std::isfinite( value.value() ) )
        {
            return badRequest( fmt::format( "parameter {} is not a finite number",
                                            TransmissionLineParameter_Name( value.parameter() ) ) );
        }

        // Resistivity is how the PCB Calculator takes conductivity; no model stores it directly
        if( param == TCP::RHO )
        {
            if( value.value() <= 0 )
                return badRequest( "TLP_RHO must be positive" );

            rho = value.value();
            continue;
        }

        if( !translineHasParam( *calc, param ) )
        {
            return badRequest( fmt::format( "parameter {} is not used by the {} model",
                                            TransmissionLineParameter_Name( value.parameter() ),
                                            TransmissionLineModel_Name( request.model() ) ) );
        }

        given[param] = value.value();
    }

    // Defaults match the PCB Calculator and the tuning profile panel
    auto setDefault =
            [&]( TCP aParam, double aValue )
            {
                if( translineHasParam( *calc, aParam ) && !given.contains( aParam ) )
                    calc->SetParameter( aParam, aValue );
            };

    setDefault( TCP::SIGMA, 1.0 / rho.value_or( copperRho ) );
    setDefault( TCP::MUR, 1.0 );
    setDefault( TCP::MURC, 1.0 );
    setDefault( TCP::EPSILON_EFF, 1.0 );
    setDefault( TCP::H_T, 1e20 );
    setDefault( TCP::FREQUENCY, 1e9 );
    setDefault( TCP::PHYS_LEN, 1.0 );
    setDefault( TCP::ANG_L, 1.0 );

    for( const auto& [param, value] : given )
        calc->SetParameter( param, value );

    if( translineHasParam( *calc, TCP::SKIN_DEPTH ) && !given.contains( TCP::SKIN_DEPTH ) )
    {
        double sigma = calc->GetParameter( TCP::SIGMA );
        double freq = calc->GetParameter( TCP::FREQUENCY );
        double murc = calc->GetParameter( TCP::MURC );

        if( sigma > 0 && freq > 0 && murc > 0 )
        {
            calc->SetParameter( TCP::SKIN_DEPTH,
                                1.0 / std::sqrt( M_PI * freq * murc * TRANSLINE_CALCULATIONS::MU0 * sigma ) );
        }
    }

    TransmissionLineResponse response;
    std::unordered_map<TCP, std::pair<double, TRANSLINE_STATUS>>* results = nullptr;

    if( request.mode() == TLMODE_SYNTHESIZE )
    {
        if( request.has_synthesis_target() )
        {
            TCP target = translineParamFromProto( request.synthesis_target() );

            if( target == TCP::UNKNOWN_ID || !translineHasParam( *calc, target ) )
                return badRequest( "the synthesis target is not a parameter of this model" );

            calc->SetSynthesizeTarget( target );
        }

        SYNTHESIZE_OPTS opts = SYNTHESIZE_OPTS::DEFAULT;

        switch( request.synthesis() )
        {
        case TLS_FIX_WIDTH:        opts = SYNTHESIZE_OPTS::FIX_WIDTH;        break;
        case TLS_FIX_SPACING:      opts = SYNTHESIZE_OPTS::FIX_SPACING;      break;
        case TLS_FROM_ZDIFF_ZCOMM: opts = SYNTHESIZE_OPTS::FROM_ZDIFF_ZCOMM; break;
        default:                   break;
        }

        response.set_converged( calc->Synthesize( opts ) );
        results = &calc->GetSynthesisResults();
    }
    else
    {
        calc->Analyse();
        response.set_converged( true );
        results = &calc->GetAnalysisResults();
    }

    std::vector<std::pair<TCP, std::pair<double, TRANSLINE_STATUS>>> sorted( results->begin(),
                                                                             results->end() );
    std::sort( sorted.begin(), sorted.end(),
               []( const auto& a, const auto& b )
               {
                   return a.first < b.first;
               } );

    for( const auto& [param, result] : sorted )
    {
        TransmissionLineResult* out = response.add_results();
        out->set_parameter( translineParamToProto( param ) );
        out->set_value( result.first );

        switch( result.second )
        {
        case TRANSLINE_STATUS::OK:       out->set_status( TLST_OK );      break;
        case TRANSLINE_STATUS::WARNING:  out->set_status( TLST_WARNING ); break;
        case TRANSLINE_STATUS::TS_ERROR: out->set_status( TLST_ERROR );   break;
        }
    }

    return response;
}
