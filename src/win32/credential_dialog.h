#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <optional>
#include <string>

namespace litefetch::ui {

// What the user typed into the credentials dialog. "Remember" is the state of
// IDC_CRED_REMEMBER; only the caller decides what to do with it, so an
// unchecked box always keeps the credential in memory for this run.
struct CredentialPrompt {
    std::string host;
    std::string username;
    std::string password;
    bool remember = false;
    // The production dialog is modal, but the test harness creates a modeless
    // instance of the same resource and closes it with DestroyWindow. EndDialog
    // is only valid for dialogs started by DialogBoxParamW.
    bool modeless = false;
};

// Shared by the modal production dialog and the in-process harness, so the
// checkbox and the answer are read by exactly one implementation.
INT_PTR CALLBACK credentialDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);

// Shows the modal credentials dialog for one host. Returns nullopt when the
// user cancels; the caller still has to reject an empty user name.
std::optional<CredentialPrompt> showCredentialDialog(HWND owner, std::string host);

// Writes the accepted answer through `save` only when the box was ticked. An
// unchecked answer never reaches storage, so nothing is on disk after a
// restart. Returns true when a credential was actually stored.
bool storeAcceptedCredential(const CredentialPrompt& prompt,
    const std::function<bool(const std::string&, const std::string&, const std::string&)>& save);

} // namespace litefetch::ui
