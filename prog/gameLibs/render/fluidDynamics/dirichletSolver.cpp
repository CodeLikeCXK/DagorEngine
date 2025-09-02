// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include <render/fluidDynamics/dirichletSolver.h>
#include <perfMon/dag_statDrv.h>
#include <drv/3d/dag_rwResource.h>
#include <drv/3d/dag_texture.h>

namespace cfd
{

// Single solver

#define DIRICHLET_VARS_LIST                   \
  VAR(cfd_tex_size)                           \
  VAR(cfd_simulation_dt)                      \
  VAR(cfd_simulation_dx)                      \
  VAR(cfd_simulation_time)                    \
  VAR(cfd_potential_tex)                      \
  VAR(cfd_initial_potential_tex)              \
  VAR(cfd_initial_potential_tex_samplerstate) \
  VAR(dirichlet_cascade_no)                   \
  VAR(dirichlet_implicit_mode)                \
  VAR(cfd_cascade_no)                         \
  VAR(cfd_calc_offset)                        \
  VAR(dirichlet_partial_solve)                \
  VAR(cfd_toroidal_offset)

#define VAR(a) static int a##VarId = -1;
DIRICHLET_VARS_LIST
#undef VAR

DirichletSolver::DirichletSolver(const char *solver_shader_name, IPoint3 tex_size, float spatial_step) :
  texSize(tex_size), spatialStep(spatial_step)
{
#define VAR(a) a##VarId = get_shader_variable_id(#a, true);
  DIRICHLET_VARS_LIST
#undef VAR

  for (int i = 0; i < 2; ++i)
  {
    potentialTex[i] = dag::create_voltex(tex_size.x, tex_size.y, tex_size.z, TEXFMT_G32R32F | TEXCF_UNORDERED, 1,
      String(0, "cfd_potential_tex_%d", i));
    d3d::resource_barrier({potentialTex[i].getBaseTex(), RB_STAGE_COMPUTE | RB_RW_UAV, 0, 0});
  }

  d3d::SamplerInfo smpInfo;
  smpInfo.address_mode_u = smpInfo.address_mode_v = smpInfo.address_mode_w = d3d::AddressMode::Mirror;
  smpInfo.filter_mode = d3d::FilterMode::Point;
  d3d::SamplerHandle smp = d3d::request_sampler(smpInfo);
  ShaderGlobal::set_sampler(::get_shader_variable_id("cfd_potential_tex_samplerstate", true), smp);
  ShaderGlobal::set_sampler(cfd_initial_potential_tex_samplerstateVarId, smp);

  ShaderGlobal::set_int4(cfd_tex_sizeVarId, IPoint4(tex_size.x, tex_size.y, tex_size.z, 0));
  initialConditionsCs.reset(new_compute_shader("dirichlet_initial_conditions"));
  solverCs.reset(new_compute_shader(solver_shader_name));
}

void DirichletSolver::fillInitialConditions()
{
  ShaderGlobal::set_texture(cfd_potential_texVarId, potentialTex[0]);
  initialConditionsCs->dispatchThreads(texSize.x, texSize.y, texSize.z);
  d3d::resource_barrier({potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
}

bool DirichletSolver::solveEquations(float dt, int num_dispatches)
{
  TIME_D3D_PROFILE(cfd_solveEquationsDirichlet);

  int currentIdx = 0;

  ShaderGlobal::set_real(cfd_simulation_dtVarId, dt * spatialStep);
  ShaderGlobal::set_real(cfd_simulation_dxVarId, spatialStep);

  solverCs->setStates();
  for (int i = 0; i < num_dispatches; ++i)
  {
    d3d::set_tex(STAGE_CS, 1, potentialTex[currentIdx].getBaseTex());
    d3d::set_rwtex(STAGE_CS, 0, potentialTex[1 - currentIdx].getBaseTex(), 0, 0);

    solverCs->dispatchThreads(texSize.x, texSize.y, texSize.z, GpuPipeline::GRAPHICS, false);

    d3d::resource_barrier({potentialTex[currentIdx].getBaseTex(), RB_STAGE_COMPUTE | RB_RW_UAV, 0, 0});
    d3d::resource_barrier({potentialTex[1 - currentIdx].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
    currentIdx = (currentIdx + 1) % 2;
    simulationTime += dt;

    ++totalNumDispatches;

    if (totalNumDispatches >= numDispatchesUntilConvergence)
    {
      resultReady = true;
      d3d::resource_barrier({potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_STAGE_PIXEL | RB_RO_SRV, 0, 0});
      return false;
    }
  }

  return true;
}

void DirichletSolver::setTotalDispatches(int num_dispatches) { numDispatchesUntilConvergence = num_dispatches; }

int DirichletSolver::getNumDispatches() const { return totalNumDispatches; }

void DirichletSolver::reset()
{
  totalNumDispatches = 0;
  simulationTime = 0.0f;
  resultReady = false;
}

bool DirichletSolver::isResultReady() const { return resultReady; }
TEXTUREID DirichletSolver::getPotentialTexId() const { return potentialTex[0].getTexId(); }

float DirichletSolver::getSimulationTime() const { return simulationTime; }

// Cascade solver

DirichletCascadeSolver::DirichletCascadeSolver(IPoint3 tex_size, float spatial_step,
  const eastl::array<uint32_t, NUM_CASCADES> &num_dispatches_per_cascade) :
  numDispatchesPerCascade(std::move(num_dispatches_per_cascade)), textureDepth(tex_size.z)
{
#define VAR(a) a##VarId = get_shader_variable_id(#a, true);
  DIRICHLET_VARS_LIST
#undef VAR

  initialConditionsCs.reset(new_compute_shader("dirichlet_initial_conditions"));
  initialConditionsFromTexCs.reset(new_compute_shader("dirichlet_initial_conditions_from_tex"));
  initialConditionsToroidalCs.reset(new_compute_shader("dirichlet_initial_conditions_toroidal"));
  explicitSolverCs.reset(new_compute_shader("dirichlet_solver"));
  implicitSolverCs.reset(new_compute_shader("dirichlet_solver_implicit"));

  for (int cascade_no = 0; cascade_no < NUM_CASCADES; ++cascade_no)
  {
    cascades[cascade_no].texSize = IPoint2(tex_size.x, tex_size.y) / (1 << cascade_no);
    cascades[cascade_no].spatialStep = spatial_step * (1 << cascade_no);
    for (int j = 0; j < 2; ++j)
    {
      cascades[cascade_no].potentialTex[j] = dag::create_voltex(cascades[cascade_no].texSize.x, cascades[cascade_no].texSize.y,
        textureDepth, TEXFMT_G32R32F | TEXCF_UNORDERED, 1, String(0, "cfd_potential_tex_cascade_%d_%d", cascade_no, j));
      d3d::resource_barrier({cascades[cascade_no].potentialTex[j].getBaseTex(), RB_STAGE_COMPUTE | RB_RW_UAV, 0, 0});
    }
  }

  d3d::SamplerInfo smpInfo;
  smpInfo.address_mode_u = smpInfo.address_mode_v = smpInfo.address_mode_w = d3d::AddressMode::Mirror;
  smpInfo.filter_mode = d3d::FilterMode::Point;
  pointSampler = d3d::request_sampler(smpInfo);
  ShaderGlobal::set_sampler(::get_shader_variable_id("cfd_potential_tex_samplerstate", true), pointSampler);
  ShaderGlobal::set_sampler(cfd_initial_potential_tex_samplerstateVarId, pointSampler);
  smpInfo.filter_mode = d3d::FilterMode::Linear;
  linearSampler = d3d::request_sampler(smpInfo);
}

void DirichletCascadeSolver::fillInitialConditions()
{
  switchToCascade(NUM_CASCADES - 1);
  initialConditionsCs->dispatchThreads(cascades[currentCascade].texSize.x, cascades[currentCascade].texSize.y, textureDepth);
  d3d::resource_barrier({cascades[currentCascade].potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
}

void DirichletCascadeSolver::fillInitialConditionsToroidal(ToroidalUpdateRegion update_region)
{
  ShaderGlobal::set_texture(cfd_initial_potential_texVarId, cascades[currentCascade].potentialTex[1]);
  ShaderGlobal::set_texture(cfd_potential_texVarId, cascades[currentCascade].potentialTex[0]);
  IPoint4 toroidalOffset = IPoint4::ZERO;
  int offsetSize = cascades[currentCascade].texSize.x / 4;
  switch (update_region)
  {
    case ToroidalUpdateRegion::TOP: toroidalOffset.y = -offsetSize; break;
    case ToroidalUpdateRegion::RIGHT: toroidalOffset.x = offsetSize; break;
    case ToroidalUpdateRegion::BOTTOM: toroidalOffset.y = offsetSize; break;
    case ToroidalUpdateRegion::LEFT: toroidalOffset.x = -offsetSize; break;
    default: G_ASSERT(0);
  }

  ShaderGlobal::set_int4(cfd_toroidal_offsetVarId, toroidalOffset);

  initialConditionsToroidalCs->dispatchThreads(cascades[currentCascade].texSize.x, cascades[currentCascade].texSize.y, textureDepth);
  d3d::resource_barrier({cascades[currentCascade].potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});

  ShaderGlobal::set_texture(cfd_initial_potential_texVarId, cascades[currentCascade].potentialTex[0]);
  ShaderGlobal::set_texture(cfd_potential_texVarId, cascades[currentCascade].potentialTex[1]);
  initialConditionsFromTexCs->dispatchThreads(cascades[currentCascade].texSize.x, cascades[currentCascade].texSize.y, textureDepth);
  d3d::resource_barrier({cascades[currentCascade].potentialTex[1].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
}

static const eastl::array<float, 3> cascade_num_factor = {0.125f, 0.5f, 1.f};
void DirichletCascadeSolver::solveEquations(float dt, int num_dispatches, bool implicit)
{
  TIME_D3D_PROFILE(cfd_solveDirichletCascade);

  if (curNumDispatches >= numDispatchesPerCascade[currentCascade])
    return;

  num_dispatches *= cascade_num_factor[currentCascade];

  const float actualDt = dt * cascades[currentCascade].spatialStep;
  ShaderGlobal::set_real(cfd_simulation_dtVarId, actualDt);
  ShaderGlobal::set_int4(cfd_tex_sizeVarId,
    IPoint4(cascades[currentCascade].texSize.x, cascades[currentCascade].texSize.y, textureDepth, 0));
  ShaderGlobal::set_real(cfd_simulation_dxVarId, cascades[currentCascade].spatialStep);

  int currentIdx = 0;
  if (!implicit)
    explicitSolverCs->setStates();
  for (int i = 0; i < num_dispatches; ++i)
  {
    d3d::set_tex(STAGE_CS, 1, cascades[currentCascade].potentialTex[currentIdx].getBaseTex());
    d3d::set_rwtex(STAGE_CS, 0, cascades[currentCascade].potentialTex[1 - currentIdx].getBaseTex(), 0, 0);

    if (implicit)
    {
      ShaderGlobal::set_int(dirichlet_cascade_noVarId, currentCascade);
      int implicitMode = currentIdx;
      ShaderGlobal::set_int(dirichlet_implicit_modeVarId, currentIdx);
      // we use groupsize of 64, so in larger cascades we process multiple pixels per thread
      implicitSolverCs->dispatchThreads(implicitMode == 0 ? 64 : cascades[currentCascade].texSize.x,
        implicitMode == 1 ? 64 : cascades[currentCascade].texSize.y, textureDepth, GpuPipeline::GRAPHICS, true);
    }
    else
      explicitSolverCs->dispatchThreads(cascades[currentCascade].texSize.x, cascades[currentCascade].texSize.y, textureDepth,
        GpuPipeline::GRAPHICS, false);

    d3d::resource_barrier({cascades[currentCascade].potentialTex[currentIdx].getBaseTex(), RB_STAGE_COMPUTE | RB_RW_UAV, 0, 0});
    d3d::resource_barrier({cascades[currentCascade].potentialTex[1 - currentIdx].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
    currentIdx = (currentIdx + 1) % 2;
    simulationTime += actualDt;

    ++totalNumDispatches;
    ++curNumDispatches;

    if (curNumDispatches >= numDispatchesPerCascade[currentCascade])
    {
      d3d::resource_barrier(
        {cascades[currentCascade].potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_STAGE_PIXEL | RB_RO_SRV, 0, 0});
      if (currentCascade == 0)
        resultReady = true;
      else
        switchToCascade(currentCascade - 1);
      break;
    }
  }
}

static const float dispatch_num_factor_partial = 0.125f;
void DirichletCascadeSolver::solveEquationsPartial(float dt, int num_dispatches, const BBox2 &area, bool implicit)
{
  TIME_D3D_PROFILE(cfd_solveDirichletCascadePartial);

  if (curNumDispatchesPartial >= numDispatchesPartial)
    return;

  num_dispatches *= dispatch_num_factor_partial;
  const float actualDt = dt * cascades[currentCascade].spatialStep;
  ShaderGlobal::set_real(cfd_simulation_dtVarId, actualDt);
  ShaderGlobal::set_int4(cfd_tex_sizeVarId,
    IPoint4(cascades[currentCascade].texSize.x, cascades[currentCascade].texSize.y, textureDepth, 0));
  ShaderGlobal::set_real(cfd_simulation_dxVarId, cascades[currentCascade].spatialStep);
  ShaderGlobal::set_int4(cfd_calc_offsetVarId,
    IPoint4(cascades[currentCascade].texSize.x * area.getMin().x, cascades[currentCascade].texSize.y * area.getMin().y, 0, 0));
  ShaderGlobal::set_int(dirichlet_partial_solveVarId, 1);

  int horCascade = 0, horThreads = 0;
  int vertCascade = 0, vertThreads = 0;
  if (implicit)
  {
    horThreads = area.size().x * cascades[currentCascade].texSize.x;
    if (is_equal_float(area.size().x, 1.0f))
      horCascade = 0;
    else
      horCascade = 2;

    vertThreads = area.size().y * cascades[currentCascade].texSize.y;
    if (is_equal_float(area.size().y, 1.0f))
      vertCascade = 0;
    else
      vertCascade = 2;
  }
  else
    explicitSolverCs->setStates();

  int currentIdx = 0;
  for (int i = 0; i < num_dispatches; ++i)
  {
    d3d::set_tex(STAGE_CS, 1, cascades[currentCascade].potentialTex[currentIdx].getBaseTex());
    d3d::set_rwtex(STAGE_CS, 0, cascades[currentCascade].potentialTex[1 - currentIdx].getBaseTex(), 0, 0);

    if (implicit)
    {
      ShaderGlobal::set_int(dirichlet_implicit_modeVarId, currentIdx);
      ShaderGlobal::set_int(dirichlet_cascade_noVarId, currentIdx == 0 ? horCascade : vertCascade); // Set cascade based on area size

      implicitSolverCs->dispatchThreads(currentIdx == 0 ? 64 : horThreads, currentIdx == 1 ? 64 : vertThreads, textureDepth);
    }
    else
      explicitSolverCs->dispatchThreads(cascades[currentCascade].texSize.x * area.size().x,
        cascades[currentCascade].texSize.y * area.size().y, textureDepth, GpuPipeline::GRAPHICS, false);

    d3d::resource_barrier({cascades[currentCascade].potentialTex[currentIdx].getBaseTex(), RB_STAGE_COMPUTE | RB_RW_UAV, 0, 0});
    d3d::resource_barrier({cascades[currentCascade].potentialTex[1 - currentIdx].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
    currentIdx = (currentIdx + 1) % 2;

    ++curNumDispatchesPartial;

    if (curNumDispatchesPartial >= numDispatchesPartial)
    {
      d3d::resource_barrier(
        {cascades[currentCascade].potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_STAGE_PIXEL | RB_RO_SRV, 0, 0});
      partialResultReady = true;
      break;
    }
  }
  ShaderGlobal::set_int(dirichlet_partial_solveVarId, 0);
}

void DirichletCascadeSolver::switchToCascade(int cascade)
{
  boundariesCb(cascade);
  ShaderGlobal::set_int(cfd_cascade_noVarId, cascade);
  ShaderGlobal::set_texture(cfd_potential_texVarId, cascades[cascade].potentialTex[0]);
  ShaderGlobal::set_int4(cfd_tex_sizeVarId, IPoint4(cascades[cascade].texSize.x, cascades[cascade].texSize.y, textureDepth, 0));

  if (cascade != NUM_CASCADES - 1)
  {
    ShaderGlobal::set_texture(cfd_initial_potential_texVarId, cascades[currentCascade].potentialTex[0]);
    ShaderGlobal::set_sampler(cfd_initial_potential_tex_samplerstateVarId, linearSampler);
    initialConditionsFromTexCs->dispatchThreads(cascades[cascade].texSize.x, cascades[cascade].texSize.y, textureDepth);
    d3d::resource_barrier({cascades[cascade].potentialTex[0].getBaseTex(), RB_STAGE_COMPUTE | RB_RO_SRV, 0, 0});
    ShaderGlobal::set_sampler(cfd_initial_potential_tex_samplerstateVarId, pointSampler);
  }

  curNumDispatches = 0;
  currentCascade = cascade;
}

void DirichletCascadeSolver::reset()
{
  totalNumDispatches = 0;
  simulationTime = 0.0f;
  curNumDispatches = 0;
  resultReady = false;
  currentCascade = NUM_CASCADES - 1;
}

bool DirichletCascadeSolver::isResultReady() const { return resultReady; }
bool DirichletCascadeSolver::isPartialResultReady() const { return partialResultReady; }
void DirichletCascadeSolver::resetPartialUpdate()
{
  curNumDispatchesPartial = 0;
  partialResultReady = false;
}
TEXTUREID DirichletCascadeSolver::getPotentialTexId() const { return cascades[currentCascade].potentialTex[0].getTexId(); }
float DirichletCascadeSolver::getSimulationTime() const { return simulationTime; }
int DirichletCascadeSolver::getNumDispatches() const { return totalNumDispatches; }
void DirichletCascadeSolver::setNumDispatchesForCascade(int cascade_no, int num_dispatches)
{
  numDispatchesPerCascade[cascade_no] = num_dispatches;
}

void DirichletCascadeSolver::setNumDispatchesPartial(int num_dispatches) { numDispatchesPartial = num_dispatches; }

void DirichletCascadeSolver::setBoundariesCb(eastl::function<void(int)> cb) { boundariesCb = std::move(cb); }
} // namespace cfd
