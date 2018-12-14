#ifndef slic3r_PrintHostSendDialog_hpp_
#define slic3r_PrintHostSendDialog_hpp_

#include <string>
#include <boost/filesystem/path.hpp>

#include <wx/string.h>
#include <wx/dialog.h>

#include "GUI.hpp"
#include "MsgDialog.hpp"


class wxTextCtrl;
class wxCheckBox;
class wxDataViewListCtrl;

namespace Slic3r {

class PrintHostJob;

namespace GUI {


class PrintHostSendDialog : public GUI::MsgDialog
{
public:
    PrintHostSendDialog(const boost::filesystem::path &path);
    boost::filesystem::path filename() const;
    bool start_print() const;

private:
    wxTextCtrl *txt_filename;
    wxCheckBox *box_print;
    bool can_start_print;
};


class PrintHostQueueDialog : public wxDialog
{
public:
    PrintHostQueueDialog(wxWindow *parent);

    void append_job(const PrintHostJob &job);

    virtual int ShowModal();
private:
    wxDataViewListCtrl *job_list;
    int prev_width;

    void sanitize_col_widths();
};


}}

#endif
