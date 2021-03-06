// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/web_contents/web_contents_view_aura.h"

#include "base/utf_string_conversions.h"
#include "content/browser/renderer_host/render_view_host_factory.h"
#include "content/browser/web_contents/interstitial_page_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_view_delegate.h"
#include "content/public/browser/web_drag_dest_delegate.h"
#include "ui/aura/client/drag_drop_client.h"
#include "ui/aura/client/drag_drop_delegate.h"
#include "ui/aura/event.h"
#include "ui/aura/root_window.h"
#include "ui/aura/window.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/dragdrop/os_exchange_data_provider_aura.h"
#include "ui/base/hit_test.h"
#include "ui/gfx/compositor/layer.h"
#include "ui/gfx/screen.h"
#include "webkit/glue/webdropdata.h"

namespace {

// Listens to all mouse drag events during a drag and drop and sends them to
// the renderer.
class WebDragSourceAura : public MessageLoopForUI::Observer {
 public:
  explicit WebDragSourceAura(WebContentsImpl* contents)
      : contents_(contents) {
    MessageLoopForUI::current()->AddObserver(this);
  }

  virtual ~WebDragSourceAura() {
    MessageLoopForUI::current()->RemoveObserver(this);
  }

  // MessageLoop::Observer implementation:
  virtual base::EventStatus WillProcessEvent(
      const base::NativeEvent& event) OVERRIDE {
    return base::EVENT_CONTINUE;
  }
  virtual void DidProcessEvent(const base::NativeEvent& event) OVERRIDE {
    ui::EventType type = ui::EventTypeFromNative(event);
    content::RenderViewHost* rvh = NULL;
    switch (type) {
      case ui::ET_MOUSE_DRAGGED:
        rvh = contents_->GetRenderViewHost();
        if (rvh) {
          gfx::Point screen_loc = ui::EventLocationFromNative(event);
          gfx::Point client_loc = screen_loc;
          aura::Window* window = rvh->GetView()->GetNativeView();
          aura::Window::ConvertPointToWindow(window->GetRootWindow(),
              window, &client_loc);
          rvh->DragSourceMovedTo(client_loc.x(), client_loc.y(),
              screen_loc.x(), screen_loc.y());
        }
        break;
      default:
        break;
    }
  }


 private:
  WebContentsImpl* contents_;

  DISALLOW_COPY_AND_ASSIGN(WebDragSourceAura);
};

// Utility to fill a ui::OSExchangeDataProviderAura object from WebDropData.
void PrepareDragData(const WebDropData& drop_data,
                     ui::OSExchangeDataProviderAura* provider) {
  if (!drop_data.plain_text.empty())
    provider->SetString(drop_data.plain_text);
  if (drop_data.url.is_valid())
    provider->SetURL(drop_data.url, drop_data.url_title);
  if (!drop_data.text_html.empty())
    provider->SetHtml(drop_data.text_html, drop_data.html_base_url);
  if (!drop_data.filenames.empty()) {
    std::vector<FilePath> paths;
    for (std::vector<string16>::const_iterator it = drop_data.filenames.begin();
        it != drop_data.filenames.end(); ++it)
      paths.push_back(FilePath::FromUTF8Unsafe(UTF16ToUTF8(*it)));
    provider->SetFilenames(paths);
  }
  if (!drop_data.custom_data.empty()) {
    Pickle pickle;
    ui::WriteCustomDataToPickle(drop_data.custom_data, &pickle);
    provider->SetPickledData(ui::Clipboard::GetWebCustomDataFormatType(),
                             pickle);
  }
}

// Utility to fill a WebDropData object from ui::OSExchangeData.
void PrepareWebDropData(WebDropData* drop_data,
                        const ui::OSExchangeData& data) {
  string16 plain_text, url_title;
  GURL url;

  data.GetString(&plain_text);
  if (!plain_text.empty())
    drop_data->plain_text = plain_text;

  data.GetURLAndTitle(&url, &url_title);
  if (url.is_valid()) {
    drop_data->url = url;
    drop_data->url_title = url_title;
  }

  data.GetHtml(&drop_data->text_html, &drop_data->html_base_url);

  std::vector<FilePath> files;
  if (data.GetFilenames(&files) && !files.empty()) {
    for (std::vector<FilePath>::const_iterator it = files.begin();
        it != files.end(); ++it)
      drop_data->filenames.push_back(UTF8ToUTF16(it->AsUTF8Unsafe()));
  }

  Pickle pickle;
  if (data.GetPickledData(ui::Clipboard::GetWebCustomDataFormatType(),
                          &pickle))
    ui::ReadCustomDataIntoMap(pickle.data(), pickle.size(),
                              &drop_data->custom_data);
}

// Utilities to convert between WebKit::WebDragOperationsMask and
// ui::DragDropTypes.
int ConvertFromWeb(WebKit::WebDragOperationsMask ops) {
  int drag_op = ui::DragDropTypes::DRAG_NONE;
  if (ops & WebKit::WebDragOperationCopy)
    drag_op |= ui::DragDropTypes::DRAG_COPY;
  if (ops & WebKit::WebDragOperationMove)
    drag_op |= ui::DragDropTypes::DRAG_MOVE;
  if (ops & WebKit::WebDragOperationLink)
    drag_op |= ui::DragDropTypes::DRAG_LINK;
  return drag_op;
}

WebKit::WebDragOperationsMask ConvertToWeb(int drag_op) {
  int web_drag_op = WebKit::WebDragOperationNone;
  if (drag_op & ui::DragDropTypes::DRAG_COPY)
    web_drag_op |= WebKit::WebDragOperationCopy;
  if (drag_op & ui::DragDropTypes::DRAG_MOVE)
    web_drag_op |= WebKit::WebDragOperationMove;
  if (drag_op & ui::DragDropTypes::DRAG_LINK)
    web_drag_op |= WebKit::WebDragOperationLink;
  return (WebKit::WebDragOperationsMask) web_drag_op;
}

}  // namespace


////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, public:

WebContentsViewAura::WebContentsViewAura(
    WebContentsImpl* web_contents,
    content::WebContentsViewDelegate* delegate)
    : web_contents_(web_contents),
      view_(NULL),
      delegate_(delegate),
      current_drag_op_(WebKit::WebDragOperationNone),
      close_tab_after_drag_ends_(false),
      drag_dest_delegate_(NULL) {
}

WebContentsViewAura::~WebContentsViewAura() {
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, private:

void WebContentsViewAura::SizeChangedCommon(const gfx::Size& size) {
  if (web_contents_->GetInterstitialPage())
    web_contents_->GetInterstitialPage()->SetSize(size);
  content::RenderWidgetHostView* rwhv =
      web_contents_->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->SetSize(size);
}

void WebContentsViewAura::EndDrag(WebKit::WebDragOperationsMask ops) {
  aura::RootWindow* root_window = GetNativeView()->GetRootWindow();
  gfx::Point screen_loc = root_window->last_mouse_location();
  gfx::Point client_loc = screen_loc;
  content::RenderViewHost* rvh = web_contents_->GetRenderViewHost();
  aura::Window* window = rvh->GetView()->GetNativeView();
  aura::Window::ConvertPointToWindow(root_window, window, &client_loc);
  rvh->DragSourceEndedAt(client_loc.x(), client_loc.y(), screen_loc.x(),
      screen_loc.y(), ops);
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, WebContentsView implementation:

void WebContentsViewAura::CreateView(const gfx::Size& initial_size) {
  initial_size_ = initial_size;

  window_.reset(new aura::Window(this));
  window_->set_owned_by_parent(false);
  window_->SetType(aura::client::WINDOW_TYPE_CONTROL);
  window_->SetTransparent(false);
  window_->Init(ui::LAYER_NOT_DRAWN);
#if defined(USE_ASH)
  window_->SetParent(NULL);
#endif
  window_->layer()->SetMasksToBounds(true);
  window_->SetName("WebContentsViewAura");

  // delegate_->GetDragDestDelegate() creates a new delegate on every call.
  // Hence, we save a reference to it locally. Similar model is used on other
  // platforms as well.
  if (delegate_.get())
    drag_dest_delegate_ = delegate_->GetDragDestDelegate();
}

content::RenderWidgetHostView* WebContentsViewAura::CreateViewForWidget(
    content::RenderWidgetHost* render_widget_host) {
  if (render_widget_host->GetView()) {
    // During testing, the view will already be set up in most cases to the
    // test view, so we don't want to clobber it with a real one. To verify that
    // this actually is happening (and somebody isn't accidentally creating the
    // view twice), we check for the RVH Factory, which will be set when we're
    // making special ones (which go along with the special views).
    DCHECK(RenderViewHostFactory::has_factory());
    return render_widget_host->GetView();
  }

  view_ = content::RenderWidgetHostView::CreateViewForWidget(
      render_widget_host);
  view_->InitAsChild(NULL);
  GetNativeView()->AddChild(view_->GetNativeView());
  view_->Show();

  // We listen to drag drop events in the newly created view's window.
  aura::client::SetDragDropDelegate(view_->GetNativeView(), this);
  return view_;
}

gfx::NativeView WebContentsViewAura::GetNativeView() const {
  return window_.get();
}

gfx::NativeView WebContentsViewAura::GetContentNativeView() const {
  return view_->GetNativeView();
}

gfx::NativeWindow WebContentsViewAura::GetTopLevelNativeWindow() const {
  return window_->GetToplevelWindow();
}

void WebContentsViewAura::GetContainerBounds(gfx::Rect *out) const {
  *out = window_->GetBoundsInRootWindow();
}

void WebContentsViewAura::SetPageTitle(const string16& title) {
  window_->set_title(title);
}

void WebContentsViewAura::OnTabCrashed(base::TerminationStatus status,
                                       int error_code) {
  view_ = NULL;
}

void WebContentsViewAura::SizeContents(const gfx::Size& size) {
  gfx::Rect bounds = window_->bounds();
  if (bounds.size() != size) {
    bounds.set_size(size);
    window_->SetBounds(bounds);
  } else {
    // Our size matches what we want but the renderers size may not match.
    // Pretend we were resized so that the renderers size is updated too.
    SizeChangedCommon(size);

  }
}

void WebContentsViewAura::RenderViewCreated(content::RenderViewHost* host) {
}

void WebContentsViewAura::Focus() {
  if (web_contents_->GetInterstitialPage()) {
    web_contents_->GetInterstitialPage()->Focus();
    return;
  }

  if (delegate_.get() && delegate_->Focus())
    return;

  content::RenderWidgetHostView* rwhv =
      web_contents_->GetRenderWidgetHostView();
  if (rwhv)
    rwhv->Focus();
}

void WebContentsViewAura::SetInitialFocus() {
  if (web_contents_->FocusLocationBarByDefault())
    web_contents_->SetFocusToLocationBar(false);
  else
    Focus();
}

void WebContentsViewAura::StoreFocus() {
  if (delegate_.get())
    delegate_->StoreFocus();
}

void WebContentsViewAura::RestoreFocus() {
  if (delegate_.get())
    delegate_->RestoreFocus();
}

bool WebContentsViewAura::IsDoingDrag() const {
  aura::RootWindow* root_window = GetNativeView()->GetRootWindow();
  if (aura::client::GetDragDropClient(root_window))
    return aura::client::GetDragDropClient(root_window)->IsDragDropInProgress();
  return false;
}

void WebContentsViewAura::CancelDragAndCloseTab() {
  DCHECK(IsDoingDrag());
  // We can't close the tab while we're in the drag and
  // |drag_handler_->CancelDrag()| is async.  Instead, set a flag to cancel
  // the drag and when the drag nested message loop ends, close the tab.
  aura::RootWindow* root_window = GetNativeView()->GetRootWindow();
  if (aura::client::GetDragDropClient(root_window))
    aura::client::GetDragDropClient(root_window)->DragCancel();

  close_tab_after_drag_ends_ = true;
}

bool WebContentsViewAura::IsEventTracking() const {
  return false;
}

void WebContentsViewAura::CloseTabAfterEventTracking() {
}

void WebContentsViewAura::GetViewBounds(gfx::Rect* out) const {
  *out = window_->GetBoundsInRootWindow();
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, RenderViewHostDelegate::View implementation:

void WebContentsViewAura::CreateNewWindow(
    int route_id,
    const ViewHostMsg_CreateWindow_Params& params) {
  web_contents_view_helper_.CreateNewWindow(web_contents_, route_id, params);
}

void WebContentsViewAura::CreateNewWidget(int route_id,
                                          WebKit::WebPopupType popup_type) {
  web_contents_view_helper_.CreateNewWidget(web_contents_,
                                            route_id,
                                            false,
                                            popup_type);
}

void WebContentsViewAura::CreateNewFullscreenWidget(int route_id) {
  web_contents_view_helper_.CreateNewWidget(web_contents_,
                                            route_id,
                                            true,
                                            WebKit::WebPopupTypeNone);
}

void WebContentsViewAura::ShowCreatedWindow(int route_id,
                                            WindowOpenDisposition disposition,
                                            const gfx::Rect& initial_pos,
                                            bool user_gesture) {
  web_contents_view_helper_.ShowCreatedWindow(
      web_contents_, route_id, disposition, initial_pos, user_gesture);
}

void WebContentsViewAura::ShowCreatedWidget(int route_id,
                                            const gfx::Rect& initial_pos) {
  web_contents_view_helper_.ShowCreatedWidget(web_contents_,
                                              route_id,
                                              false,
                                              initial_pos);
}

void WebContentsViewAura::ShowCreatedFullscreenWidget(int route_id) {
  web_contents_view_helper_.ShowCreatedWidget(web_contents_,
                                              route_id,
                                              true,
                                              gfx::Rect());
}

void WebContentsViewAura::ShowContextMenu(
    const content::ContextMenuParams& params) {
  // Allow WebContentsDelegates to handle the context menu operation first.
  if (web_contents_->GetDelegate() &&
      web_contents_->GetDelegate()->HandleContextMenu(params)) {
    return;
  }

  if (delegate_.get())
    delegate_->ShowContextMenu(params);
}

void WebContentsViewAura::ShowPopupMenu(const gfx::Rect& bounds,
                                        int item_height,
                                        double item_font_size,
                                        int selected_item,
                                        const std::vector<WebMenuItem>& items,
                                        bool right_aligned) {
  // External popup menus are only used on Mac.
  NOTIMPLEMENTED();
}

void WebContentsViewAura::StartDragging(
    const WebDropData& drop_data,
    WebKit::WebDragOperationsMask operations,
    const SkBitmap& image,
    const gfx::Point& image_offset) {
  aura::RootWindow* root_window = GetNativeView()->GetRootWindow();
  if (!aura::client::GetDragDropClient(root_window))
    return;

  ui::OSExchangeDataProviderAura* provider = new ui::OSExchangeDataProviderAura;
  PrepareDragData(drop_data, provider);
  if (!image.isNull()) {
    provider->set_drag_image(image);
    provider->set_drag_image_offset(image_offset);
  }
  ui::OSExchangeData data(provider);  // takes ownership of |provider|.

  scoped_ptr<WebDragSourceAura> drag_source(
      new WebDragSourceAura(web_contents_));

  // We need to enable recursive tasks on the message loop so we can get
  // updates while in the system DoDragDrop loop.
  int result_op = 0;
  {
    // TODO(sad): Avoid using last_mouse_location here, since the drag may not
    // always start from a mouse-event (e.g. a touch or gesture event could
    // initiate the drag). The location information should be carried over from
    // webkit. http://crbug.com/114754
    gfx::Point location(root_window->last_mouse_location());
    MessageLoop::ScopedNestableTaskAllower allow(MessageLoop::current());
    result_op = aura::client::GetDragDropClient(root_window)->StartDragAndDrop(
        data, location, ConvertFromWeb(operations));
  }

  EndDrag(ConvertToWeb(result_op));
  web_contents_->GetRenderViewHost()->DragSourceSystemDragEnded();
}

void WebContentsViewAura::UpdateDragCursor(WebKit::WebDragOperation operation) {
  current_drag_op_ = operation;
}

void WebContentsViewAura::GotFocus() {
  if (web_contents_->GetDelegate())
    web_contents_->GetDelegate()->WebContentsFocused(web_contents_);
}

void WebContentsViewAura::TakeFocus(bool reverse) {
  if (web_contents_->GetDelegate() &&
      !web_contents_->GetDelegate()->TakeFocus(reverse) &&
      delegate_.get()) {
    delegate_->TakeFocus(reverse);
  }
}

////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, aura::WindowDelegate implementation:

gfx::Size WebContentsViewAura::GetMinimumSize() const {
  return gfx::Size();
}

void WebContentsViewAura::OnBoundsChanged(const gfx::Rect& old_bounds,
                                          const gfx::Rect& new_bounds) {
  SizeChangedCommon(new_bounds.size());
  if (delegate_.get())
    delegate_->SizeChanged(new_bounds.size());
}

void WebContentsViewAura::OnFocus() {
}

void WebContentsViewAura::OnBlur() {
}

bool WebContentsViewAura::OnKeyEvent(aura::KeyEvent* event) {
  return false;
}

gfx::NativeCursor WebContentsViewAura::GetCursor(const gfx::Point& point) {
  return gfx::kNullCursor;
}

int WebContentsViewAura::GetNonClientComponent(const gfx::Point& point) const {
  return HTCLIENT;
}

bool WebContentsViewAura::OnMouseEvent(aura::MouseEvent* event) {
  if (!web_contents_->GetDelegate())
    return false;

  switch (event->type()) {
    case ui::ET_MOUSE_PRESSED:
      web_contents_->GetDelegate()->ActivateContents(web_contents_);
      break;
    case ui::ET_MOUSE_MOVED:
      web_contents_->GetDelegate()->ContentsMouseEvent(
          web_contents_, gfx::Screen::GetCursorScreenPoint(), true);
      break;
    default:
      break;
  }
  return false;
}

ui::TouchStatus WebContentsViewAura::OnTouchEvent(aura::TouchEvent* event) {
  return ui::TOUCH_STATUS_UNKNOWN;
}

ui::GestureStatus WebContentsViewAura::OnGestureEvent(
    aura::GestureEvent* event) {
  return ui::GESTURE_STATUS_UNKNOWN;
}

bool WebContentsViewAura::CanFocus() {
  return true;
}

void WebContentsViewAura::OnCaptureLost() {
}

void WebContentsViewAura::OnPaint(gfx::Canvas* canvas) {
}

void WebContentsViewAura::OnWindowDestroying() {
}

void WebContentsViewAura::OnWindowDestroyed() {
}

void WebContentsViewAura::OnWindowVisibilityChanged(bool visible) {
  if (visible)
    web_contents_->ShowContents();
  else
    web_contents_->HideContents();
}
////////////////////////////////////////////////////////////////////////////////
// WebContentsViewAura, aura::client::DragDropDelegate implementation:

void WebContentsViewAura::OnDragEntered(const aura::DropTargetEvent& event) {
  if (drag_dest_delegate_)
    drag_dest_delegate_->DragInitialize(web_contents_);

  WebDropData drop_data;
  PrepareWebDropData(&drop_data, event.data());
  WebKit::WebDragOperationsMask op = ConvertToWeb(event.source_operations());

  gfx::Point screen_pt =
      GetNativeView()->GetRootWindow()->last_mouse_location();
  web_contents_->GetRenderViewHost()->DragTargetDragEnter(
      drop_data, event.location(), screen_pt, op);

  if (drag_dest_delegate_) {
    drag_dest_delegate_->OnReceiveDragData(event.data());
    drag_dest_delegate_->OnDragEnter();
  }
}

int WebContentsViewAura::OnDragUpdated(const aura::DropTargetEvent& event) {
  WebKit::WebDragOperationsMask op = ConvertToWeb(event.source_operations());
  gfx::Point screen_pt =
      GetNativeView()->GetRootWindow()->last_mouse_location();
  web_contents_->GetRenderViewHost()->DragTargetDragOver(
      event.location(), screen_pt, op);

  if (drag_dest_delegate_)
    drag_dest_delegate_->OnDragOver();

  return ConvertFromWeb(current_drag_op_);
}

void WebContentsViewAura::OnDragExited() {
  web_contents_->GetRenderViewHost()->DragTargetDragLeave();
  if (drag_dest_delegate_)
    drag_dest_delegate_->OnDragLeave();
}

int WebContentsViewAura::OnPerformDrop(const aura::DropTargetEvent& event) {
  web_contents_->GetRenderViewHost()->DragTargetDrop(
      event.location(),
      GetNativeView()->GetRootWindow()->last_mouse_location());
  if (drag_dest_delegate_)
    drag_dest_delegate_->OnDrop();
  return current_drag_op_;
}
