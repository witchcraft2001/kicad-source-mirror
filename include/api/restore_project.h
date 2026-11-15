// Local document-restore parsing context; GPL-3.0-or-later.
#ifndef KICAD_RESTORE_PROJECT_H
#define KICAD_RESTORE_PROJECT_H
#include <project.h>
#include <project/project_file.h>
#include <project/project_local_settings.h>
class API_RESTORE_PROJECT : public PROJECT
{
public:
    explicit API_RESTORE_PROJECT( const wxString& aPath ) : m_file( aPath ), m_local( this, aPath )
    {
        setProjectFullName( aPath );
        setProjectFile( &m_file );
        setLocalSettings( &m_local );
        m_file.LoadFromFile();
    }
private:
    PROJECT_FILE m_file;
    PROJECT_LOCAL_SETTINGS m_local;
};
#endif
