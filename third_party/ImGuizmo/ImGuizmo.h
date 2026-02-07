#pragma once
// Minimal ImGuizmo-compatible subset for SandboxCityEngine.
// Provides translate/rotate/scale gizmos for ImGui editors.

#include <imgui.h>

namespace ImGuizmo
{
  enum OPERATION
  {
    TRANSLATE = 1,
    ROTATE = 2,
    SCALE = 4
  };

  enum MODE
  {
    LOCAL,
    WORLD
  };

  void SetOrthographic(bool is_ortho);
  void SetDrawlist(ImDrawList* drawlist = nullptr);
  void SetRect(float x, float y, float width, float height);

  bool Manipulate(const float* view,
                  const float* projection,
                  OPERATION operation,
                  MODE mode,
                  float* matrix,
                  float* deltaMatrix = nullptr,
                  const float* snap = nullptr,
                  const float* localBounds = nullptr,
                  const float* boundsSnap = nullptr);

  bool IsOver();
  bool IsUsing();

  void DecomposeMatrixToComponents(const float* matrix,
                                   float* translation,
                                   float* rotation,
                                   float* scale);

  void RecomposeMatrixFromComponents(const float* translation,
                                     const float* rotation,
                                     const float* scale,
                                     float* matrix);
}
