#include "PrintHostDialogs.hpp"

#include <algorithm>

#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/wupdlock.h>
#include <wx/debug.h>

#include "GUI.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "../Utils/PrintHost.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {


PrintHostSendDialog::PrintHostSendDialog(const fs::path &path)
    : MsgDialog(nullptr, _(L("Send G-Code to printer host")), _(L("Upload to Printer Host with the following filename:")), wxID_NONE)
    , txt_filename(new wxTextCtrl(this, wxID_ANY, path.filename().wstring()))
    , box_print(new wxCheckBox(this, wxID_ANY, _(L("Start printing after upload"))))
{
    auto *label_dir_hint = new wxStaticText(this, wxID_ANY, _(L("Use forward slashes ( / ) as a directory separator if needed.")));
    label_dir_hint->Wrap(CONTENT_WIDTH);

    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);
    content_sizer->Add(box_print, 0, wxBOTTOM, 2*VERT_SPACING);

    btn_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL));

    txt_filename->SetFocus();
    wxString stem(path.stem().wstring());
    txt_filename->SetSelection(0, stem.Length());

    Fit();
}

fs::path PrintHostSendDialog::filename() const
{
    return fs::path(txt_filename->GetValue().wx_str());
}

bool PrintHostSendDialog::start_print() const
{
    return box_print->GetValue();
}



PrintHostQueueDialog::PrintHostQueueDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _(L("Print host upload queue")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , prev_width(0)
{
    enum { HEIGHT = 800, WIDTH = 400, SPACING = 5 };

    SetMinSize(wxSize(HEIGHT, WIDTH));

    auto *topsizer = new wxBoxSizer(wxVERTICAL);

    job_list = new wxDataViewListCtrl(this, wxID_ANY);
    job_list->AppendTextColumn("Host", wxDATAVIEW_CELL_INERT);
    job_list->AppendTextColumn("Filename", wxDATAVIEW_CELL_INERT);
    job_list->AppendTextColumn("Status", wxDATAVIEW_CELL_INERT);
    job_list->AppendProgressColumn("Progress", wxDATAVIEW_CELL_INERT);

    // XXX:
    wxVector<wxVariant> data;
    data.push_back(wxVariant("foobar.local"));
    data.push_back(wxVariant("barbaz.gcode"));
    data.push_back(wxVariant("enqueued"));
    data.push_back(wxVariant(0));
    job_list->AppendItem(data);
    data.clear();
    data.push_back(wxVariant("quzqux.local"));
    data.push_back(wxVariant("frobnicator.gcode"));
    data.push_back(wxVariant("uploading"));
    data.push_back(wxVariant(50));
    job_list->AppendItem(data);

    auto *btnsizer = new wxBoxSizer(wxHORIZONTAL);
    auto *btn_cancel = new wxButton(this, wxID_DELETE, _(L("Cancel selected")));
    auto *btn_close = new wxButton(this, wxID_CANCEL, _(L("Close")));
    btnsizer->Add(btn_cancel, 0, wxRIGHT, SPACING);
    btnsizer->AddStretchSpacer();
    btnsizer->Add(btn_close);

    topsizer->Add(job_list, 1, wxEXPAND | wxBOTTOM, SPACING);
    topsizer->Add(btnsizer, 0, wxEXPAND);
    SetSizer(topsizer);

    job_list->Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
        CallAfter([this]() { sanitize_col_widths(); });
    });
}

void PrintHostQueueDialog::append_job(const PrintHostJob &job)
{
    wxCHECK_RET(!job.empty(), "PrintHostQueueDialog: Attempt to append an empty job");

    wxVector<wxVariant> fields;
    fields.push_back(wxVariant(job.printhost->get_host()));
    fields.push_back(wxVariant(job.upload_data.upload_path.string()));
    fields.push_back(wxVariant("Enqueued"));
    fields.push_back(wxVariant(0));
    job_list->AppendItem(fields);
}

int PrintHostQueueDialog::ShowModal()
{
    CallAfter([this]() { sanitize_col_widths(); });
    return wxDialog::ShowModal();
}

void PrintHostQueueDialog::sanitize_col_widths()
{
    enum { PRORGESS_SIZE = 100 };

    const int width = job_list->GetSize().GetWidth();

    auto col_host = job_list->GetColumn(0);
    auto col_file = job_list->GetColumn(1);
    auto col_status = job_list->GetColumn(2);
    auto col_progress = job_list->GetColumn(3);

    // This ensure column shrinking when the window is shrinked
    const int fix_shrink = std::min(width - prev_width, 0);
    col_file->SetWidth(col_file->GetWidth() + fix_shrink);

    // Here we ensure that progress field is PRORGESS_SIZE wide and we detect extra width byeond that
    // The extra width (if any) is moved to the filename field
    const int extra_size = col_progress->GetWidth() - PRORGESS_SIZE;
    if (extra_size > 0) {
        col_file->SetWidth(col_file->GetWidth() + extra_size);
        col_progress->SetWidth(PRORGESS_SIZE);
    }

    prev_width = width;
}


}}
