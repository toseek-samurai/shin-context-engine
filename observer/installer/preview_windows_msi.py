"""Preview one MSI dialog without installing or executing package actions."""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('msi', type=Path)
    parser.add_argument('--dialog', default='WelcomeDlg', help='For example: LicenseAgreementDlg, VerifyReadyDlg, ProgressDlg, ExitDialog')
    parser.add_argument('--seconds', type=int, default=60, help='Automatically close the preview after this interval (1-300)')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 300:
        parser.error('--seconds must be between 1 and 300')
    msi, user32 = ctypes.WinDLL('msi'), ctypes.WinDLL('user32')
    msi.MsiOpenDatabaseW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p, ctypes.POINTER(wintypes.UINT)]
    msi.MsiEnableUIPreview.argtypes = [wintypes.UINT, ctypes.POINTER(wintypes.UINT)]
    msi.MsiPreviewDialogW.argtypes = [wintypes.UINT, wintypes.LPCWSTR]
    msi.MsiCloseHandle.argtypes = [wintypes.UINT]
    user32.SetTimer.argtypes = [wintypes.HWND, ctypes.c_size_t, wintypes.UINT, ctypes.c_void_p]
    user32.SetTimer.restype = ctypes.c_size_t
    user32.KillTimer.argtypes = [wintypes.HWND, ctypes.c_size_t]
    user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
    user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
    user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
    user32.DispatchMessageW.restype = ctypes.c_ssize_t

    def check(code):
        if code:
            raise OSError(code, ctypes.FormatError(code))

    database, preview = wintypes.UINT(), wintypes.UINT()
    # MSIDBOPEN_READONLY. Never use MsiOpenPackage/MsiInstallProduct here.
    check(msi.MsiOpenDatabaseW(str(args.msi.resolve()), None, ctypes.byref(database)))
    try:
        check(msi.MsiEnableUIPreview(database, ctypes.byref(preview)))
        try:
            check(msi.MsiPreviewDialogW(preview, args.dialog))
            timer = user32.SetTimer(None, 0, args.seconds * 1000, None)
            if not timer:
                raise ctypes.WinError()
            try:
                print(f'Preview only: {args.dialog}. No installation actions run.', flush=True)
                message = wintypes.MSG()
                while True:
                    result = user32.GetMessageW(ctypes.byref(message), None, 0, 0)
                    if result == -1:
                        raise ctypes.WinError()
                    if result == 0 or (message.message == 0x113 and message.wParam == timer):
                        break
                    user32.TranslateMessage(ctypes.byref(message))
                    user32.DispatchMessageW(ctypes.byref(message))
            finally:
                user32.KillTimer(None, timer)
        finally:
            msi.MsiCloseHandle(preview)
    finally:
        msi.MsiCloseHandle(database)


if __name__ == '__main__':
    main()
