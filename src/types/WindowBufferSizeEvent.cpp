// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "inc/IInputEvent.hpp"

WindowBufferSizeEvent::~WindowBufferSizeEvent()
{
}

INPUT_RECORD WindowBufferSizeEvent::ToInputRecord() const noexcept
{
    INPUT_RECORD record{ 0 };
    record.EventType = WINDOW_BUFFER_SIZE_EVENT;
    record.Event.WindowBufferSizeEvent.dwSize.X = gsl::narrow_cast<short>(std::clamp(_size.X, 0, SHRT_MAX));
    record.Event.WindowBufferSizeEvent.dwSize.Y = gsl::narrow_cast<short>(std::clamp(_size.Y, 0, SHRT_MAX));
    return record;
}

InputEventType WindowBufferSizeEvent::EventType() const noexcept
{
    return InputEventType::WindowBufferSizeEvent;
}

void WindowBufferSizeEvent::SetSize(const til::size size) noexcept
{
    _size = size;
}
