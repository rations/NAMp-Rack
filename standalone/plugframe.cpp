// PlugFrame implementation. See plugframe.h.

#include "plugframe.h"

using namespace Steinberg;

namespace Rations
{

//------------------------------------------------------------------------
tresult PLUGIN_API PlugFrame::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;

    // The run loop is the shared one, and it is handed back through this frame because that is the
    // only route a plug-in has to it: IPlugView::setFrame is the single host pointer a view holds.
    if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid))
        return mLoop.queryInterface(iid, obj);

    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        addRef();
        return kResultOk;
    }

    *obj = nullptr;
    return kNoInterface;
}

//------------------------------------------------------------------------
tresult PLUGIN_API PlugFrame::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize)
        return kInvalidArgument;
    if (!mResize)
        return kResultFalse;
    return mResize(view, newSize) ? kResultTrue : kResultFalse;
}

} // namespace Rations
