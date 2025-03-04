// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

// Some of the interactivity classes pulled in by ServiceLocator.hpp are not
// yet audit-safe, so for now we'll need to disable a bunch of warnings.
#pragma warning(disable : 26432)
#pragma warning(disable : 26440)
#pragma warning(disable : 26455)

#include "InteractDispatch.hpp"
#include "conGetSet.hpp"
#include "../../host/conddkrefs.h"
#include "../../interactivity/inc/ServiceLocator.hpp"
#include "../../interactivity/inc/EventSynthesis.hpp"
#include "../../types/inc/Viewport.hpp"
#include "../../inc/unicode.hpp"

using namespace Microsoft::Console::Interactivity;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;

// takes ownership of pConApi
InteractDispatch::InteractDispatch(std::unique_ptr<ConGetSet> pConApi) :
    _pConApi(std::move(pConApi))
{
    THROW_HR_IF_NULL(E_INVALIDARG, _pConApi.get());
}

// Method Description:
// - Writes a collection of input to the host. The new input is appended to the
//      end of the input buffer.
//  If Ctrl+C is written with this function, it will not trigger a Ctrl-C
//      interrupt in the client, but instead write a Ctrl+C to the input buffer
//      to be read by the client.
// Arguments:
// - inputEvents: a collection of IInputEvents
// Return Value:
// - True.
bool InteractDispatch::WriteInput(std::deque<std::unique_ptr<IInputEvent>>& inputEvents)
{
    size_t written = 0;
    _pConApi->WriteInput(inputEvents, written);
    return true;
}

// Method Description:
// - Writes a key event to the host in a fashion that will enable the host to
//   process special keys such as Ctrl-C or Ctrl+Break. The host will then
//   decide what to do with it, including potentially sending an interrupt to a
//   client application.
// Arguments:
// - event: The key to send to the host.
// Return Value:
// - True.
bool InteractDispatch::WriteCtrlKey(const KeyEvent& event)
{
    HandleGenericKeyEvent(event, false);
    return true;
}

// Method Description:
// - Writes a string of input to the host. The string is converted to keystrokes
//      that will faithfully represent the input by CharToKeyEvents.
// Arguments:
// - string : a string to write to the console.
// Return Value:
// - True.
bool InteractDispatch::WriteString(const std::wstring_view string)
{
    if (!string.empty())
    {
        const auto codepage = _pConApi->GetConsoleOutputCP();
        std::deque<std::unique_ptr<IInputEvent>> keyEvents;

        for (const auto& wch : string)
        {
            auto convertedEvents = CharToKeyEvents(wch, codepage);

            std::move(convertedEvents.begin(),
                      convertedEvents.end(),
                      std::back_inserter(keyEvents));
        }

        WriteInput(keyEvents);
    }
    return true;
}

//Method Description:
// Window Manipulation - Performs a variety of actions relating to the window,
//      such as moving the window position, resizing the window, querying
//      window state, forcing the window to repaint, etc.
//  This is kept separate from the output version, as there may be
//      codes that are supported in one direction but not the other.
//Arguments:
// - function - An identifier of the WindowManipulation function to perform
// - parameter1 - The first optional parameter for the function
// - parameter2 - The second optional parameter for the function
// Return value:
// True if handled successfully. False otherwise.
bool InteractDispatch::WindowManipulation(const DispatchTypes::WindowManipulationType function,
                                          const VTParameter parameter1,
                                          const VTParameter parameter2)
{
    // Other Window Manipulation functions:
    //  MSFT:13271098 - QueryViewport
    //  MSFT:13271146 - QueryScreenSize
    switch (function)
    {
    case DispatchTypes::WindowManipulationType::DeIconifyWindow:
        _pConApi->ShowWindow(true);
        return true;
    case DispatchTypes::WindowManipulationType::IconifyWindow:
        _pConApi->ShowWindow(false);
        return true;
    case DispatchTypes::WindowManipulationType::RefreshWindow:
        _pConApi->GetTextBuffer().TriggerRedrawAll();
        return true;
    case DispatchTypes::WindowManipulationType::ResizeWindowInCharacters:
        // TODO:GH#1765 We should introduce a better `ResizeConpty` function to
        // the ConGetSet interface, that specifically handles a conpty resize.
        if (_pConApi->ResizeWindow(parameter2.value_or(0), parameter1.value_or(0)))
        {
            auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
            THROW_IF_FAILED(gci.GetVtIo()->SuppressResizeRepaint());
        }
        return true;
    default:
        return false;
    }
}

//Method Description:
// Move Cursor: Moves the cursor to the provided VT coordinates. This is the
//      coordinate space where 1,1 is the top left cell of the viewport.
//Arguments:
// - row: The row to move the cursor to.
// - col: The column to move the cursor to.
// Return value:
// - True.
bool InteractDispatch::MoveCursor(const VTInt row, const VTInt col)
{
    // First retrieve some information about the buffer
    const auto viewport = _pConApi->GetViewport();

    // In VT, the origin is 1,1. For our array, it's 0,0. So subtract 1.
    // Apply boundary tests to ensure the cursor isn't outside the viewport rectangle.
    til::point coordCursor{ col - 1 + viewport.Left, row - 1 + viewport.Top };
    coordCursor.Y = std::clamp(coordCursor.Y, viewport.Top, viewport.Bottom);
    coordCursor.X = std::clamp(coordCursor.X, viewport.Left, viewport.Right);

    const auto coordCursorShort = til::unwrap_coord(coordCursor);

    // MSFT: 15813316 - Try to use this MoveCursor call to inherit the cursor position.
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    RETURN_IF_FAILED(gci.GetVtIo()->SetCursorPosition(coordCursorShort));

    // Finally, attempt to set the adjusted cursor position back into the console.
    auto& cursor = _pConApi->GetTextBuffer().GetCursor();
    cursor.SetPosition(coordCursorShort);
    cursor.SetHasMoved(true);
    return true;
}

// Routine Description:
// - Checks if the InputBuffer is willing to accept VT Input directly
// Arguments:
// - <none>
// Return value:
// - true if enabled (see IsInVirtualTerminalInputMode). false otherwise.
bool InteractDispatch::IsVtInputEnabled() const
{
    return _pConApi->IsVtInputEnabled();
}

// Method Description:
// - Inform the console that the window is focused. This is used by ConPTY.
//   Terminals can send ConPTY a FocusIn/FocusOut sequence on the input pipe,
//   which will end up here. This will update the console's internal tracker if
//   it's focused or not, as to match the end-terminal's state.
// - Used to call ConsoleControl(ConsoleSetForeground,...).
// Arguments:
// - focused: if the terminal is now focused
// Return Value:
// - true always.
bool InteractDispatch::FocusChanged(const bool focused) const
{
    auto& g = ServiceLocator::LocateGlobals();
    auto& gci = g.getConsoleInformation();

    // This should likely always be true - we shouldn't ever have an
    // InteractDispatch outside ConPTY mode, but just in case...
    if (gci.IsInVtIoMode())
    {
        auto shouldActuallyFocus = false;

        // From https://github.com/microsoft/terminal/pull/12799#issuecomment-1086289552
        // Make sure that the process that's telling us it's focused, actually
        // _is_ in the FG. We don't want to allow malicious.exe to say "yep I'm
        // in the foreground, also, here's a popup" if it isn't actually in the
        // FG.
        if (focused)
        {
            if (const auto pseudoHwnd{ ServiceLocator::LocatePseudoWindow() })
            {
                // They want focus, we found a pseudo hwnd.

                // Note: ::GetParent(pseudoHwnd) will return 0. GetAncestor works though.
                // GA_PARENT and GA_ROOT seemingly return the same thing for
                // Terminal. We're going with GA_ROOT since it seems
                // semantically more correct here.
                if (const auto ownerHwnd{ ::GetAncestor(pseudoHwnd, GA_ROOT) })
                {
                    // We have an owner from a previous call to ReparentWindow

                    if (const auto currentFgWindow{ ::GetForegroundWindow() })
                    {
                        // There is a window in the foreground (it's possible there
                        // isn't one)

                        // Get the PID of the current FG window, and compare with our owner's PID.
                        DWORD currentFgPid{ 0 };
                        DWORD ownerPid{ 0 };
                        GetWindowThreadProcessId(currentFgWindow, &currentFgPid);
                        GetWindowThreadProcessId(ownerHwnd, &ownerPid);

                        if (ownerPid == currentFgPid)
                        {
                            // Huzzah, the app that owns us is actually the FG
                            // process. They're allowed to grand FG rights.
                            shouldActuallyFocus = true;
                        }
                    }
                }
            }
        }

        WI_UpdateFlag(gci.Flags, CONSOLE_HAS_FOCUS, shouldActuallyFocus);
        gci.ProcessHandleList.ModifyConsoleProcessFocus(shouldActuallyFocus);
        gci.pInputBuffer->Write(std::make_unique<FocusEvent>(focused));
    }
    // Does nothing outside of ConPTY. If there's a real HWND, then the HWND is solely in charge.

    return true;
}
