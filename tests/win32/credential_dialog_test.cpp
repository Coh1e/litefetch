// The credentials dialog answer: the checkbox has to reach the caller.
//
// The product dialog is modal, so the test creates a modeless instance of the
// same resource with the same dialog procedure, ticks the box and sends IDOK.
// That drives the real answer-reading code instead of re-implementing it.
#include "credential_dialog.h"

#include "credentials.h"
#include "resource.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace litefetch;
using namespace litefetch::test;
using namespace litefetch::ui;

namespace {

class DialogInstance {
public:
    explicit DialogInstance(CredentialPrompt& prompt) : prompt_(prompt)
    {
        prompt_.modeless = true;
        handle_ = CreateDialogParamW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDD_CREDENTIALS), nullptr, credentialDialogProc,
            reinterpret_cast<LPARAM>(&prompt_));
        if (!handle_) throw std::runtime_error("cannot create the credentials dialog");
    }
    ~DialogInstance() { close(); }
    DialogInstance(const DialogInstance&) = delete;
    DialogInstance& operator=(const DialogInstance&) = delete;

    HWND handle() const { return handle_; }

    void setText(int id, const wchar_t* text) { SetDlgItemTextW(handle_, id, text); }

    void setRemember(bool checked)
    {
        CheckDlgButton(handle_, IDC_CRED_REMEMBER, checked ? BST_CHECKED : BST_UNCHECKED);
    }

    bool rememberChecked() const
    {
        return IsDlgButtonChecked(handle_, IDC_CRED_REMEMBER) == BST_CHECKED;
    }

    std::wstring text(int id) const
    {
        wchar_t buffer[512]{};
        GetDlgItemTextW(handle_, id, buffer, static_cast<int>(std::size(buffer)));
        return buffer;
    }

    void sendCommand(int id)
    {
        SendMessageW(handle_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
            reinterpret_cast<LPARAM>(GetDlgItem(handle_, id)));
    }

    void close()
    {
        if (handle_ && IsWindow(handle_)) DestroyWindow(handle_);
        handle_ = nullptr;
    }

private:
    CredentialPrompt& prompt_;
    HWND handle_ = nullptr;
};

void checkedRememberIsPartOfTheAnswer()
{
    CredentialPrompt prompt;
    prompt.host = "example.org";
    DialogInstance dialog(prompt);
    check(!dialog.rememberChecked(), "remember starts unchecked");
    dialog.setText(IDC_CRED_USER, L"alice");
    dialog.setText(IDC_CRED_PASS, L"s3cret");
    dialog.setRemember(true);
    dialog.sendCommand(IDOK);

    check(prompt.remember, "the ticked box reaches the answer");
    checkEqual(prompt.username, "alice", "the user name reaches the answer");
    checkEqual(prompt.password, "s3cret", "the password reaches the answer");
    checkEqual(prompt.host, "example.org", "the host is carried through unchanged");
    check(!IsWindow(dialog.handle()), "confirming closes the dialog");
}

void uncheckedRememberStaysInMemory()
{
    CredentialPrompt prompt;
    DialogInstance dialog(prompt);
    dialog.setText(IDC_CRED_USER, L"bob");
    dialog.setText(IDC_CRED_PASS, L"hunter2");
    dialog.sendCommand(IDOK);

    check(!prompt.remember, "an unticked box keeps the credential in memory");
    checkEqual(prompt.username, "bob", "the user name is still read");
    checkEqual(prompt.password, "hunter2", "the password is still read");
    check(!IsWindow(dialog.handle()), "the dialog closes without remembering");
}

void cancelReportsNoAnswer()
{
    CredentialPrompt prompt;
    DialogInstance dialog(prompt);
    dialog.setText(IDC_CRED_USER, L"alice");
    dialog.setText(IDC_CRED_PASS, L"s3cret");
    dialog.setRemember(true);
    dialog.sendCommand(IDCANCEL);

    check(prompt.username.empty() && prompt.password.empty(),
        "a cancelled dialog must not hand over typed credentials");
    check(!prompt.remember, "a cancelled dialog has no remember decision");
    check(!IsWindow(dialog.handle()), "cancel closes the dialog");
}

void hintMatchesTheCheckbox()
{
    CredentialPrompt prompt;
    DialogInstance dialog(prompt);
    const std::wstring hint = dialog.text(IDC_CRED_HINT);
    // The old copy promised the credential would never be saved while the
    // checkbox offered exactly that. Both halves must now match the behaviour.
    check(hint.find(L"Windows 保护存储") != std::wstring::npos,
        "the hint names the protected store used when the box is ticked");
    check(hint.find(L"不勾选只用于本次运行") != std::wstring::npos,
        "the hint explains the memory-only default");
    check(hint.find(L"不会保存") == std::wstring::npos,
        "the hint must not claim credentials are never saved");
}

void checkedAnswerIsStoredWithoutPlaintext(const std::filesystem::path& root)
{
    const auto file = root / "checked.dat";
    CredentialStore store(file);
    CredentialPrompt prompt;
    prompt.host = "example.org";
    prompt.username = "alice";
    prompt.password = "s3cret";
    prompt.remember = true;

    bool called = false;
    const bool stored = storeAcceptedCredential(prompt,
        [&](const std::string& host, const std::string& username, const std::string& password) {
            called = true;
            return store.save(host, username, password);
        });
    check(called, "a ticked answer reaches storage");
    check(stored, "a ticked answer is reported as stored");

    // A new store instance is the next start: the DPAPI blob has to load.
    CredentialStore restarted(file);
    const auto loaded = restarted.load("example.org");
    check(loaded.has_value(), "the remembered credential survives the restart");
    checkEqual(loaded->username, "alice", "the user name survives the restart");
    checkEqual(loaded->password, "s3cret", "the password survives the restart");

    std::ifstream input(file, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    check(raw.find("s3cret") == std::string::npos,
        "the credential file never contains the password in clear");
}

void uncheckedAnswerIsNotStored(const std::filesystem::path& root)
{
    const auto file = root / "unchecked.dat";
    CredentialStore store(file);
    CredentialPrompt prompt;
    prompt.host = "example.org";
    prompt.username = "alice";
    prompt.password = "s3cret";
    prompt.remember = false;

    bool called = false;
    const bool stored = storeAcceptedCredential(prompt,
        [&](const std::string& host, const std::string& username, const std::string& password) {
            called = true;
            return store.save(host, username, password);
        });
    check(!called, "an unticked answer never reaches storage");
    check(!stored, "an unticked answer reports nothing stored");
    check(!std::filesystem::exists(file), "an unticked answer writes no credential file");
}

} // namespace

int main()
{
    return run("ui-credential", [] {
        TempDir root;
        checkedRememberIsPartOfTheAnswer();
        uncheckedRememberStaysInMemory();
        cancelReportsNoAnswer();
        hintMatchesTheCheckbox();
        checkedAnswerIsStoredWithoutPlaintext(root.path());
        uncheckedAnswerIsNotStored(root.path());
        return 0;
    });
}
