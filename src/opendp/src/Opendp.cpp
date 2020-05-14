/////////////////////////////////////////////////////////////////////////////
// Original authors: SangGi Do(sanggido@unist.ac.kr), Mingyu
// Woo(mwoo@eng.ucsd.edu)
//          (respective Ph.D. advisors: Seokhyeong Kang, Andrew B. Kahng)
// Rewrite by James Cherry, Parallax Software, Inc.
//
// BSD 3-Clause License
//
// Copyright (c) 2019, James Cherry, Parallax Software, Inc.
// Copyright (c) 2018, SangGi Do and Mingyu Woo
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
///////////////////////////////////////////////////////////////////////////////

#include "opendp/Opendp.h"
#include <cfloat>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <ostream>
#include "openroad/Error.hh"
#include "openroad/OpenRoad.hh"  // closestPtInRect

namespace opendp {

using std::ifstream;
using std::ofstream;
using std::round;
using std::string;

using ord::error;

using odb::dbBox;
using odb::dbBPin;
using odb::dbBTerm;
using odb::dbITerm;
using odb::dbMasterType;
using odb::dbMPin;
using odb::dbMTerm;
using odb::dbNet;
using odb::dbPlacementStatus;
using odb::Rect;

Cell::Cell() :
  db_inst_(nullptr),
  x_(0),
  y_(0),
  width_(0),
  height_(0),
  is_placed_(false),
  hold_(false),
  group_(nullptr),
  region_(nullptr)
{
}

const char *
Cell::name() const
{
  return db_inst_->getConstName();
}

int64_t
Cell::area() const
{
  dbMaster *master = db_inst_->getMaster();
  return master->getWidth() * master->getHeight();
}

////////////////////////////////////////////////////////////////

bool
Opendp::isFixed(const Cell *cell) const
{
  return cell == &dummy_cell_ || cell->db_inst_->isFixed();
}

bool
Opendp::isMultiRow(const Cell *cell) const
{
  auto iter = db_master_map_.find(cell->db_inst_->getMaster());
  assert(iter != db_master_map_.end());
  return iter->second.is_multi_row_;
}

Power
Opendp::topPower(const Cell *cell) const
{
  auto iter = db_master_map_.find(cell->db_inst_->getMaster());
  assert(iter != db_master_map_.end());
  return iter->second.top_power_;
}

////////////////////////////////////////////////////////////////

Group::Group() :
  util(0.0)
{
}

Opendp::Opendp() :
  pad_right_(0),
  pad_left_(0),
  power_net_name_("VDD"),
  ground_net_name_("VSS"),
  grid_(nullptr)
{
  dummy_cell_.is_placed_ = true;
  // magic number alert
  diamond_search_height_ = 100;
  diamond_search_width_ = diamond_search_height_ * 5;
  max_displacement_constraint_ = 0;
}

Opendp::~Opendp()
{
  deleteGrid(grid_);
}

void
Opendp::init(dbDatabase *db)
{
  db_ = db;
}

void
Opendp::setPowerNetName(const char *power_name)
{
  power_net_name_ = power_name;
}

void
Opendp::setGroundNetName(const char *ground_name)
{
  ground_net_name_ = ground_name;
}

void
Opendp::setPaddingGlobal(int left, int right)
{
  pad_left_ = left;
  pad_right_ = right;
}

bool
Opendp::havePadding() const
{
  return pad_left_ > 0 || pad_right_ > 0;
}

void
Opendp::detailedPlacement(int max_displacment)
{
  importDb();
  reportImportWarnings();
  findDesignStats();
  max_displacement_constraint_ = max_displacment;
  reportDesignStats();
  int64_t hpwl_before = hpwl();
  detailedPlacement();
  int64_t avg_displacement, sum_displacement, max_displacement;
  displacementStats(&avg_displacement, &sum_displacement, &max_displacement);
  updateDbInstLocations();
  reportLegalizationStats(hpwl_before, avg_displacement,
			  sum_displacement, max_displacement);
}

void
Opendp::updateDbInstLocations()
{
  for (Cell &cell : cells_) {
    if (!isFixed(&cell) && isStdCell(&cell)) {
      dbInst *db_inst_ = cell.db_inst_;
      db_inst_->setOrient(cell.orient_);
      db_inst_->setLocation(core_.xMin() + cell.x_, core_.yMin() + cell.y_);
    }
  }
}

void
Opendp::findDesignStats()
{
  fixed_inst_count_ = 0;
  fixed_area_ = 0;
  fixed_padded_area_ = 0;
  movable_area_ = 0;
  movable_padded_area_ = 0;
  max_cell_height_ = 0;

  for (Cell &cell : cells_) {
    int64_t cell_area = cell.area();
    int64_t cell_padded_area = paddedArea(&cell);
    if (isFixed(&cell)) {
      fixed_area_ += cell_area;
      fixed_padded_area_ += cell_padded_area;
      fixed_inst_count_++;
    }
    else {
      movable_area_ += cell_area;
      movable_padded_area_ += cell_padded_area;
      int cell_height = gridNearestHeight(&cell);
      if (cell_height > max_cell_height_) {
        max_cell_height_ = cell_height;
      }
    }
  }

  design_area_ = row_count_ * static_cast<int64_t>(row_site_count_) * site_width_ * row_height_;

  design_util_ = static_cast<double>(movable_area_) / (design_area_ - fixed_area_);

  design_padded_util_ = static_cast<double>(movable_padded_area_) / (design_area_ - fixed_padded_area_);

  if (design_util_ > 1.0) {
    error("utilization exceeds 100%.");
  }
}

void
Opendp::reportDesignStats() const
{
  printf("Design Stats\n");
  printf("--------------------------------\n");
  printf("total instances      %8d\n", block_->getInsts().size());
  printf("multi row instances  %8d\n", multi_row_inst_count_);
  printf("fixed instances      %8d\n", fixed_inst_count_);
  printf("nets                 %8d\n", block_->getNets().size());
  printf("design area          %8.1f u^2\n", dbuAreaToMicrons(design_area_));
  printf("fixed area           %8.1f u^2\n", dbuAreaToMicrons(fixed_area_));
  printf("movable area         %8.1f u^2\n", dbuAreaToMicrons(movable_area_));
  printf("utilization          %8.0f %%\n", design_util_ * 100);
  printf("utilization padded   %8.0f %%\n", design_padded_util_ * 100);
  printf("rows                 %8d\n", row_count_);
  printf("row height           %8.1f u\n", dbuToMicrons(row_height_));
  if (max_cell_height_ > 1) {
    printf("max height           %8d rows\n", max_cell_height_);
  }
  if (groups_.size() > 0) {
    printf("group count          %8lu\n", groups_.size());
  }
  printf("\n");
}

void
Opendp::reportLegalizationStats(int64_t hpwl_before,
				int64_t avg_displacement,
				int64_t sum_displacement,
				int64_t max_displacement) const
{
  printf("Placement Analysis\n");
  printf("--------------------------------\n");
  printf("total displacement   %8.1f u\n", dbuToMicrons(sum_displacement));
  printf("average displacement %8.1f u\n", dbuToMicrons(avg_displacement));
  printf("max displacement     %8.1f u\n", dbuToMicrons(max_displacement));
  printf("original HPWL        %8.1f u\n", dbuToMicrons(hpwl_before));
  double hpwl_legal = hpwl();
  printf("legalized HPWL       %8.1f u\n", dbuToMicrons(hpwl_legal));
  double hpwl_delta = (hpwl_legal - hpwl_before) / hpwl_before * 100;
  printf("delta HPWL           %8.0f %%\n", hpwl_delta);
  printf("\n");
}

////////////////////////////////////////////////////////////////

void
Opendp::displacementStats(// Return values.
			  int64_t *avg_displacement,
			  int64_t *sum_displacement,
			  int64_t *max_displacement) const
{
  *avg_displacement = 0;
  *sum_displacement = 0;
  *max_displacement = 0;

  for (const Cell &cell : cells_) {
    int displacement = disp(&cell);
    *sum_displacement += displacement;
    if (displacement > *max_displacement) {
      *max_displacement = displacement;
    }
  }
  *avg_displacement = *sum_displacement / cells_.size();
}

// Note that this does NOT use cell/core coordinates.
int64_t
Opendp::hpwl() const
{
  int64_t hpwl = 0;
  for (dbNet *net : block_->getNets()) {
    Rect box;
    box.mergeInit();

    for (dbITerm *iterm : net->getITerms()) {
      int x, y;
      if (iterm->getAvgXY(&x, &y)) {
	Rect iterm_rect(x, y, x, y);
	box.merge(iterm_rect);
      }
      else {
	// This clause is sort of worthless because getAvgXY prints
	// a warning when it fails.
	dbInst *inst = iterm->getInst();
	dbBox *bbox = inst->getBBox();
	int center_x = (bbox->xMin() + bbox->xMax()) / 2;
	int center_y = (bbox->yMin() + bbox->yMax()) / 2;
	Rect inst_center(center_x, center_y, center_x, center_y);
	box.merge(inst_center);
      }
    }

    for (dbBTerm *bterm : net->getBTerms()) {
      for (dbBPin *bpin : bterm->getBPins()) {
        dbPlacementStatus status = bpin->getPlacementStatus();
        if (status.isPlaced()) {
          dbBox *pin_box = bpin->getBox();
          Rect pin_rect;
          pin_box->getBox(pin_rect);
          int center_x = (pin_rect.xMin() + pin_rect.xMax()) / 2;
          int center_y = (pin_rect.yMin() + pin_rect.yMax()) / 2;
          Rect pin_center(center_x, center_y, center_x, center_y);
          box.merge(pin_center);
        }
      }
    }
    int perimeter = box.dx() + box.dy();
    hpwl += perimeter;
  }
  return hpwl;
}

////////////////////////////////////////////////////////////////

Power
Opendp::rowTopPower(int row) const
{
  return ((row0_top_power_is_vdd_ ? row : row + 1) % 2 == 0) ? VDD : VSS;
}

dbOrientType
Opendp::rowOrient(int row) const
{
  // Row orient flips R0 -> MX -> R0 -> MX ...
  return ((row0_orient_is_r0_ ? row : row + 1) % 2 == 0) ? dbOrientType::R0
                                                         : dbOrientType::MX;
}

////////////////////////////////////////////////////////////////

void
Opendp::initialLocation(const Cell *cell,
                        // Return values.
                        int *x,
                        int *y) const
{
  initialLocation(cell->db_inst_, x, y);
}

void
Opendp::initialLocation(const dbInst *inst,
                        // Return values.
                        int *x,
                        int *y) const
{
  int loc_x, loc_y;
  inst->getLocation(loc_x, loc_y);
  *x = loc_x - core_.xMin();
  *y = loc_y - core_.yMin();
}

void
Opendp::initialPaddedLocation(const Cell *cell,
                              // Return values.
                              int *x,
                              int *y) const
{
  initialLocation(cell, x, y);
  if (isPadded(cell)) {
    *x -= pad_left_ * site_width_;
  }
}

int
Opendp::disp(const Cell *cell) const
{
  int init_x, init_y;
  initialLocation(cell, &init_x, &init_y);
  return abs(init_x - cell->x_) + abs(init_y - cell->y_);
}

bool
Opendp::isPaddedType(const Cell *cell) const
{
  dbMasterType type = cell->db_inst_->getMaster()->getType();
  // Use switch so if new types are added we get a compiler warning.
  switch (type) {
    case dbMasterType::CORE:
    case dbMasterType::CORE_ANTENNACELL:
    case dbMasterType::CORE_FEEDTHRU:
    case dbMasterType::CORE_TIEHIGH:
    case dbMasterType::CORE_TIELOW:
    case dbMasterType::CORE_WELLTAP:
    case dbMasterType::ENDCAP:
    case dbMasterType::ENDCAP_PRE:
    case dbMasterType::ENDCAP_POST:
      return true;
    case dbMasterType::CORE_SPACER:
    case dbMasterType::BLOCK:
    case dbMasterType::BLOCK_BLACKBOX:
    case dbMasterType::BLOCK_SOFT:
    case dbMasterType::ENDCAP_TOPLEFT:
    case dbMasterType::ENDCAP_TOPRIGHT:
    case dbMasterType::ENDCAP_BOTTOMLEFT:
    case dbMasterType::ENDCAP_BOTTOMRIGHT:
      // These classes are completely ignored by the placer.
    case dbMasterType::COVER:
    case dbMasterType::COVER_BUMP:
    case dbMasterType::RING:
    case dbMasterType::PAD:
    case dbMasterType::PAD_AREAIO:
    case dbMasterType::PAD_INPUT:
    case dbMasterType::PAD_OUTPUT:
    case dbMasterType::PAD_INOUT:
    case dbMasterType::PAD_POWER:
    case dbMasterType::PAD_SPACER:
    case dbMasterType::NONE:
      return false;
  }
  // gcc warniing
  return false;
}

bool
Opendp::isStdCell(const Cell *cell) const
{
  dbMasterType type = cell->db_inst_->getMaster()->getType();
  // Use switch so if new types are added we get a compiler warning.
  switch (type) {
    case dbMasterType::CORE:
    case dbMasterType::CORE_ANTENNACELL:
    case dbMasterType::CORE_FEEDTHRU:
    case dbMasterType::CORE_TIEHIGH:
    case dbMasterType::CORE_TIELOW:
    case dbMasterType::CORE_SPACER:
    case dbMasterType::CORE_WELLTAP:
      return true;
    case dbMasterType::BLOCK:
    case dbMasterType::BLOCK_BLACKBOX:
    case dbMasterType::BLOCK_SOFT:
    case dbMasterType::ENDCAP:
    case dbMasterType::ENDCAP_PRE:
    case dbMasterType::ENDCAP_POST:
    case dbMasterType::ENDCAP_TOPLEFT:
    case dbMasterType::ENDCAP_TOPRIGHT:
    case dbMasterType::ENDCAP_BOTTOMLEFT:
    case dbMasterType::ENDCAP_BOTTOMRIGHT:
      // These classes are completely ignored by the placer.
    case dbMasterType::COVER:
    case dbMasterType::COVER_BUMP:
    case dbMasterType::RING:
    case dbMasterType::PAD:
    case dbMasterType::PAD_AREAIO:
    case dbMasterType::PAD_INPUT:
    case dbMasterType::PAD_OUTPUT:
    case dbMasterType::PAD_INOUT:
    case dbMasterType::PAD_POWER:
    case dbMasterType::PAD_SPACER:
    case dbMasterType::NONE:
      return false;
  }
  // gcc warniing
  return false;
}

/* static */
bool
Opendp::isBlock(const Cell *cell)
{
  dbMasterType type = cell->db_inst_->getMaster()->getType();
  return type == dbMasterType::BLOCK;
}

int
Opendp::gridEndX() const
{
  return divCeil(core_.dx(), site_width_);
}

int
Opendp::gridEndY() const
{
  return divCeil(core_.dy(), row_height_);
}

int
Opendp::paddedWidth(const Cell *cell) const
{
  if (isPadded(cell)) {
    return cell->width_ + (pad_left_ + pad_right_) * site_width_;
  }
  return cell->width_;
}

bool
Opendp::isPadded(const Cell *cell) const
{
  return isPaddedType(cell) && (pad_left_ > 0 || pad_right_ > 0);
}

int
Opendp::gridPaddedWidth(const Cell *cell) const
{
  return divCeil(paddedWidth(cell), site_width_);
}

int
Opendp::gridHeight(const Cell *cell) const
{
  return divCeil(cell->height_, row_height_);
}

int64_t
Opendp::paddedArea(const Cell *cell) const
{
  return paddedWidth(cell) * cell->height_;
}

// Callers should probably be using gridPaddedWidth.
int
Opendp::gridNearestWidth(const Cell *cell) const
{
  return divRound(paddedWidth(cell), site_width_);
}

// Callers should probably be using gridHeight.
int
Opendp::gridNearestHeight(const Cell *cell) const
{
  return divRound(cell->height_, row_height_);
}

int
Opendp::gridX(int x) const
{
  return x / site_width_;
}

int
Opendp::gridY(int y) const
{
  return y / row_height_;
}

int
Opendp::gridX(const Cell *cell) const
{
  return gridX(cell->x_);
}

int
Opendp::gridPaddedX(const Cell *cell) const
{
  if (isPadded(cell)) {
    return gridX(cell->x_ - pad_left_ * site_width_);
  }
  return gridX(cell->x_);
}

int
Opendp::gridY(const Cell *cell) const
{
  return gridY(cell->y_);
}

void
Opendp::setGridPaddedLoc(Cell *cell, int x, int y) const
{
  cell->x_ = (x + (isPadded(cell) ? pad_left_ : 0)) * site_width_;
  cell->y_ = y * row_height_;
}

int
Opendp::gridPaddedEndX(const Cell *cell) const
{
  return divCeil(
      cell->x_ + cell->width_ + (isPadded(cell) ? pad_right_ * site_width_ : 0),
      site_width_);
}

int
Opendp::gridEndX(const Cell *cell) const
{
  return divCeil(cell->x_ + cell->width_, site_width_);
}

int
Opendp::gridEndY(const Cell *cell) const
{
  return divCeil(cell->y_ + cell->height_, row_height_);
}

int
Opendp::coreGridMaxX() const
{
  return divRound(core_.xMax(), site_width_);
}

int
Opendp::coreGridMaxY() const
{
  return divRound(core_.yMax(), row_height_);
}

double
Opendp::dbuToMicrons(int64_t dbu) const
{
  double dbu_micron = db_->getTech()->getDbUnitsPerMicron();
  return dbu / dbu_micron;
}

double
Opendp::dbuAreaToMicrons(int64_t dbu_area) const
{
  double dbu_micron = db_->getTech()->getDbUnitsPerMicron();
  return dbu_area / (dbu_micron * dbu_micron);
}

int
divRound(int dividend, int divisor)
{
  return round(static_cast<double>(dividend) / divisor);
}

int
divCeil(int dividend, int divisor)
{
  return ceil(static_cast<double>(dividend) / divisor);
}

int
divFloor(int dividend, int divisor)
{
  return dividend / divisor;
}

void
Opendp::reportGrid()
{
  importDb();
  const Grid *grid = makeCellGrid();
  reportGrid(grid);
}

void
Opendp::reportGrid(const Grid *grid) const
{
  std::map<const Cell *, int> cell_index;
  int i = 0;
  for (const Cell &cell : cells_) {
    cell_index[&cell] = i;
    i++;
  }

  // column header
  printf("   ");
  for (int j = 0; j < row_site_count_; j++) {
    printf("|%3d", j);
  }
  printf("|\n");
  printf("   ");
  for (int j = 0; j < row_site_count_; j++) {
    printf("|---");
  }
  printf("|\n");

  for (int i = row_count_ - 1; i >= 0; i--) {
    printf("%3d", i);
    for (int j = 0; j < row_site_count_; j++) {
      const Cell *cell = grid[i][j].cell;
      if (cell != nullptr) {
        printf("|%3d", cell_index[cell]);
      }
      else {
        printf("|   ");
      }
    }
    printf("|\n");
  }
  printf("\n");

  i = 0;
  for (const Cell &cell : cells_) {
    printf("%3d %s\n", i, cell.name());
    i++;
  }
}

}  // namespace opendp
