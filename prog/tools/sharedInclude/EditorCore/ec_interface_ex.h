// Copyright (C) Gaijin Games KFT.  All rights reserved.
#pragma once

#include <EditorCore/ec_interface.h>
#include <scene/dag_visibility.h>

// external classes forward declaration
class DataBlock;
class IGenSave;
class String;

// Interface for VisibilityFinder
class IVisibilityFinderProvider
{
public:
  static constexpr unsigned HUID = 0x509D8ABEu; // IVisibilityFinderProvider
  virtual VisibilityFinder &getVisibilityFinder() = 0;
};

// Interface to perform staged 3D rendering
class IRenderingService
{
public:
  static constexpr unsigned HUID = 0x80C30E29u; // IRenderingService
  enum Stage
  {
    STG_BEFORE_RENDER,
    STG_RENDER_SHADOWS,
    STG_RENDER_ENVI,
    STG_RENDER_STATIC_OPAQUE,
    STG_RENDER_STATIC_DECALS,
    STG_RENDER_DYNAMIC_OPAQUE,
    STG_RENDER_DYNAMIC_DECALS,
    STG_RENDER_STATIC_TRANS,
    STG_RENDER_DYNAMIC_TRANS,
    STG_RENDER_STATIC_DISTORTION,
    STG_RENDER_DYNAMIC_DISTORTION,
    STG_RENDER_FX_LOWRES,
    STG_RENDER_FX,
    STG_RENDER_FX_DISTORTION,
    STG_RENDER_SHADOWS_VSM,
    STG_RENDER_SHADOWS_FOM,
    STG_RENDER_TO_CLIPMAP,
    STG_RENDER_TO_CLIPMAP_LATE,

    STG_RENDER_HEIGHT_FIELD,
    STG_RENDER_GRASS_MASK,
    STG_RENDER_CLOUDS,
    STG_RENDER_LAND_DECALS,

    STG_RENDER_WATER_PROJ,

    STG_RENDER_HEIGHT_PATCH,
  };

  virtual void renderGeometry(Stage stage) = 0;
  virtual void renderUI() {}
  virtual int setSubDiv(int) { return 0; }
  virtual void prepare([[maybe_unused]] const Point3 &center_pos, [[maybe_unused]] const BBox3 &box) {}
};


class IMainWindowImguiRenderingService
{
public:
  static constexpr unsigned HUID = 0x30B1B0D1u;

  // Called before ImGui::NewFrame.
  virtual void beforeUpdateImgui() {}

  virtual void updateImgui() {}
};


// Interface to exchange environment changes in BLK form
class IEnvironmentSettings
{
public:
  static constexpr unsigned HUID = 0x62B95DA4u; // IEnvironmentSettings
  virtual void getEnvironmentSettings(DataBlock &blk) = 0;
  virtual void setEnvironmentSettings(DataBlock &blk) = 0;
};


// Interface to get notifications when HDR settings changed
class IHDRChangeSettingsClient
{
public:
  static constexpr unsigned HUID = 0x006003ABu; // IHDRChangeSettingsClient
  virtual void onHDRSettingsChanged() = 0;
};


/// Interface class for dialog window 'Select objects by name'.\n
/// (Called by key 'H')
/// @ingroup EditorCore
/// @ingroup Misc
/// @ingroup SelectByName
class IObjectsList
{
public:
  /// Get objects names.
  /// The function passes object names to dialog window.
  /// @param[out] names - list of objects names
  /// @param[out] sel_names - list of selected objects names
  /// @param[in] types - list of object types to be represented. Defined as
  ///         a list of indexes. To get full list of types use getTypeNames().
  virtual void getObjNames(Tab<String> &names, Tab<String> &sel_names, const Tab<int> &types) = 0;

  /// Get objects types.
  /// The function passes object types to dialog window.
  /// @param[out] names - list of objects types
  virtual void getTypeNames(Tab<String> &names) = 0;

  /// Informs client code about selecting / deselecting objects in list.
  /// @param[in] names - names list of selected objects
  virtual void onSelectedNames(const Tab<String> &names) = 0;
};


/// Wrapper for IGenEventHandler.
/// The class is intended for simplification of event handling in
/// IGenEventHandler.
/// Inheriting from this class one may override not all but wanted functions of
/// IGenEventHandler only. Other events will be skipped or handled by
/// event handler obtained by getWrappedHandler().
/// @ingroup EditorCore
/// @ingroup EditorBaseClasses
/// @ingroup EventHandler
class IGenEventHandlerWrapper : public IGenEventHandler
{
public:
  /// Get wrapped handler.
  /// Get pointer to default event handler.
  /// @return pointer to default event handler
  virtual IGenEventHandler *getWrappedHandler() = 0;

  //*******************************************************
  ///@name Mouse events handlers.
  //@{
  /// Handles mouse move.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseMove()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseMove()
  bool handleMouseMove(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseMove(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse left button press.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseLBPress()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseLBPress()
  bool handleMouseLBPress(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseLBPress(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse left button release.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseLBRelease()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseLBRelease()
  bool handleMouseLBRelease(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseLBRelease(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse right button press.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseRBPress()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseRBPress()
  bool handleMouseRBPress(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseRBPress(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse right button release.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseRBRelease()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseRBRelease()
  bool handleMouseRBRelease(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseRBRelease(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse center button press.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseCBPress()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseCBPress()
  bool handleMouseCBPress(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseCBPress(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse center button release.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseCBRelease()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseCBRelease()
  bool handleMouseCBRelease(IGenViewportWnd *wnd, int x, int y, bool inside, int buttons, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseCBRelease(wnd, x, y, inside, buttons, key_modif);
    return false;
  }

  /// Handles mouse scroll wheel.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseWheel()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseWheel()
  bool handleMouseWheel(IGenViewportWnd *wnd, int wheel_d, int x, int y, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseWheel(wnd, wheel_d, x, y, key_modif);
    return false;
  }

  /// Handles mouse double-click.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleMouseDoubleClick()) will be called.
  /// In other case the function returns false.
  /// @copydoc IGenEventHandler::handleMouseDoubleClick()
  bool handleMouseDoubleClick(IGenViewportWnd *wnd, int x, int y, int key_modif) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      return h->handleMouseDoubleClick(wnd, x, y, key_modif);
    return false;
  }
  //@}

  //*******************************************************
  ///@name Viewport redraw/change events handlers.
  //@{
  /// Viewport CTL window redraw.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleViewportPaint()) will be called.
  /// In other case the function does nothing.
  /// @copydoc IGenEventHandler::handleViewportPaint()
  void handleViewportPaint(IGenViewportWnd *wnd) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      h->handleViewportPaint(wnd);
  }

  /// Viewport view change notification.
  /// If getWrappedHandler() returns not NULL then corresponding method
  /// (%getWrappedHandler()->%handleViewChange()) will be called.
  /// In other case the function does nothing.
  /// @copydoc IGenEventHandler::handleViewChange()
  void handleViewChange(IGenViewportWnd *wnd) override
  {
    IGenEventHandler *h = getWrappedHandler();
    if (h)
      h->handleViewChange(wnd);
  }
  //@}
};


/// Interface class for viewport to operate with custom cameras.
/// @ingroup EditorCore
/// @ingroup Cameras
class ICustomCameras
{
public:
  /// Get current camera ID.
  /// @return current camera ID. If no such a camera the function
  ///         @b must return -1
  virtual int getSelectedCamera() = 0;

  /// Get camera properties.
  /// @param[in] id - camera ID (menu ID of the camera)
  /// @param[out] matrix - camera matrix
  /// @param[out] fov - camera FOV (<b>F</b>ield <b>O</b>f <b>V</b>iew)
  /// @param[out] name - camera name
  /// @return @b true if operation successful, @b false in other case
  virtual bool getCamera(const int id, TMatrix &matrix, real &fov, String &name) = 0;

  /// Set camera matrix.
  /// @param[in] id - camera ID (menu ID of the camera)
  /// @param[in] matrix - camera matrix
  /// @return @b true if operation successful, @b false in other case
  virtual bool setCamera(const int id, const TMatrix &matrix) = 0;
};


/// Interface class for name generation routine.
/// Used during objects name generation in Editor. All plugins that create named
/// objects support this interface. The Editor may poll all plugins
/// by means of INamespace to know if an object name be unique.
/// @ingroup EditorCore
/// @ingroup Misc
class INamespace
{
public:
  /// Test whether object name is unique for current plugin.
  /// Called by Editor.
  /// @param[in] name - tested name
  /// @return @b true if name is unique for current plugin,
  ///         @b false in other case
  virtual bool isUniqName(const char *name) = 0;
};
