/////////////////////////////////////////////////////////////////////////////
// Original authors: SangGi Do(sanggido@unist.ac.kr), Mingyu
// Woo(mwoo@eng.ucsd.edu)
//          (respective Ph.D. advisors: Seokhyeong Kang, Andrew B. Kahng)
// Rewrite by James Cherry, Parallax Software, Inc.
//
// Copyright (c) 2019, The Regents of the University of California
// Copyright (c) 2018, SangGi Do and Mingyu Woo
// All rights reserved.
//
// BSD 3-Clause License
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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

#include "DplObserver.h"
#include "dpl/Opendp.h"
#include "utl/Logger.h"

// #define ODP_DEBUG

namespace dpl {

using std::abs;
using std::max;
using std::min;
using std::numeric_limits;
using std::sort;
using std::string;
using std::vector;

using utl::DPL;

void Opendp::detailedPlacement()
{
  if (debug_observer_) {
    debug_observer_->startPlacement(block_);
  }

  placement_failures_.clear();
  initGrid();
  // Paint fixed cells.
  setFixedGridCells();
  // group mapping & x_axis dummycell insertion
  groupInitPixels2();
  // y axis dummycell insertion
  groupInitPixels();

  if (!groups_.empty()) {
    placeGroups();
  }
  place();

  if (debug_observer_) {
    debug_observer_->endPlacement();
  }
}

////////////////////////////////////////////////////////////////

void Opendp::placeGroups()
{
  groupAssignCellRegions();

  prePlaceGroups();
  prePlace();

  // naive placement method ( multi -> single )
  placeGroups2();
  for (Group& group : groups_) {
    // magic number alert
    for (int pass = 0; pass < 3; pass++) {
      int refine_count = groupRefine(&group);
      int anneal_count = anneal(&group);
      // magic number alert
      if (refine_count < 10 || anneal_count < 100) {
        break;
      }
    }
  }
}

void Opendp::prePlace()
{
  for (Cell& cell : cells_) {
    Rect* group_rect = nullptr;
    if (!cell.inGroup() && !cell.is_placed_) {
      for (Group& group : groups_) {
        for (Rect& rect : group.regions) {
          if (checkOverlap(&cell, &rect)) {
            group_rect = &rect;
          }
        }
      }
      if (group_rect) {
        Point nearest = nearestPt(&cell, group_rect);
        Point legal = legalGridPt(&cell, nearest);
        debugPrint(logger_,
                   DPL,
                   "place",
                   4,
                   "preplace {} {} {}",
                   cell.name(),
                   legal.getX(),
                   legal.getY());
        if (mapMove(&cell, legal)) {
          cell.hold_ = true;
        }
      }
    }
  }
}

bool Opendp::checkOverlap(const Cell* cell, const Rect* rect) const
{
  Point init = initialLocation(cell, false);
  int x = init.getX();
  int y = init.getY();
  return x + cell->width_ > rect->xMin() && x < rect->xMax()
         && y + cell->height_ > rect->yMin() && y < rect->yMax();
}

Point Opendp::nearestPt(const Cell* cell, const Rect* rect) const
{
  Point init = initialLocation(cell, false);
  int x = init.getX();
  int y = init.getY();

  int temp_x = x;
  int temp_y = y;

  int cell_width = cell->width_;
  if (checkOverlap(cell, rect)) {
    int dist_x, dist_y;
    if (abs(x + cell_width - rect->xMin()) > abs(rect->xMax() - x)) {
      dist_x = abs(rect->xMax() - x);
      temp_x = rect->xMax();
    } else {
      dist_x = abs(x - rect->xMin());
      temp_x = rect->xMin() - cell_width;
    }
    if (abs(y + cell->height_ - rect->yMin()) > abs(rect->yMax() - y)) {
      dist_y = abs(rect->yMax() - y);
      temp_y = rect->yMax();
    } else {
      dist_y = abs(y - rect->yMin());
      temp_y = rect->yMin() - cell->height_;
    }
    if (dist_x < dist_y) {
      return Point(temp_x, y);
    }
    return Point(x, temp_y);
  }

  if (x < rect->xMin()) {
    temp_x = rect->xMin();
  } else if (x + cell_width > rect->xMax()) {
    temp_x = rect->xMax() - cell_width;
  }

  if (y < rect->yMin()) {
    temp_y = rect->yMin();
  } else if (y + cell->height_ > rect->yMax()) {
    temp_y = rect->yMax() - cell->height_;
  }

  return Point(temp_x, temp_y);
}

void Opendp::prePlaceGroups()
{
  for (Group& group : groups_) {
    for (Cell* cell : group.cells_) {
      if (!isFixed(cell) && !cell->is_placed_) {
        int dist = numeric_limits<int>::max();
        bool in_group = false;
        Rect* nearest_rect = nullptr;
        for (Rect& rect : group.regions) {
          if (isInside(cell, &rect)) {
            in_group = true;
          }
          int rect_dist = distToRect(cell, &rect);
          if (rect_dist < dist) {
            dist = rect_dist;
            nearest_rect = &rect;
          }
        }
        if (!nearest_rect) {
          continue;  // degenerate case of empty group.regions
        }
        if (!in_group) {
          Point nearest = nearestPt(cell, nearest_rect);
          Point legal = legalGridPt(cell, nearest);
          debugPrint(logger_,
                     DPL,
                     "place",
                     4,
                     "preplace groups {} {} {}",
                     cell->name(),
                     legal.getX(),
                     legal.getY());

          if (mapMove(cell, legal)) {
            cell->hold_ = true;
          }
        }
      }
    }
  }
}

bool Opendp::isInside(const Cell* cell, const Rect* rect) const
{
  Point init = initialLocation(cell, false);
  int x = init.getX();
  int y = init.getY();
  return x >= rect->xMin() && x + cell->width_ <= rect->xMax()
         && y >= rect->yMin() && y + cell->height_ <= rect->yMax();
}

int Opendp::distToRect(const Cell* cell, const Rect* rect) const
{
  Point init = initialLocation(cell, true);
  int x = init.getX();
  int y = init.getY();

  int dist_x = 0;
  int dist_y = 0;
  if (x < rect->xMin()) {
    dist_x = rect->xMin() - x;
  } else if (x + cell->width_ > rect->xMax()) {
    dist_x = x + cell->width_ - rect->xMax();
  }

  if (y < rect->yMin()) {
    dist_y = rect->yMin() - y;
  } else if (y + cell->height_ > rect->yMax()) {
    dist_y = y + cell->height_ - rect->yMax();
  }

  return dist_y + dist_x;
}

class CellPlaceOrderLess
{
 public:
  explicit CellPlaceOrderLess(Opendp* opendp);
  bool operator()(const Cell* cell1, const Cell* cell2) const;

 private:
  int centerDist(const Cell* cell) const;

  int center_x_;
  int center_y_;
};

CellPlaceOrderLess::CellPlaceOrderLess(Opendp* opendp)
{
  Rect core = opendp->getCore();
  center_x_ = (core.xMin() + core.xMax()) / 2;
  center_y_ = (core.yMin() + core.yMax()) / 2;
}

int CellPlaceOrderLess::centerDist(const Cell* cell) const
{
  return abs(cell->x_ - center_x_) + abs(cell->y_ - center_y_);
}

bool CellPlaceOrderLess::operator()(const Cell* cell1, const Cell* cell2) const
{
  int64_t area1 = cell1->area();
  int64_t area2 = cell2->area();
  int dist1 = centerDist(cell1);
  int dist2 = centerDist(cell2);
  return area1 > area2
         || (area1 == area2
             && (dist1 < dist2
                 || (dist1 == dist2
                     && strcmp(cell1->db_inst_->getConstName(),
                               cell2->db_inst_->getConstName())
                            < 0)));
}

void Opendp::place()
{
  vector<Cell*> sorted_cells;
  sorted_cells.reserve(cells_.size());

  for (Cell& cell : cells_) {
    if (!(isFixed(&cell) || cell.inGroup() || cell.is_placed_)) {
      sorted_cells.push_back(&cell);
      if (!cellFitsInCore(&cell)) {
        logger_->error(DPL,
                       15,
                       "instance {} does not fit inside the ROW core area.",
                       cell.name());
      }
    }
  }
  sort(sorted_cells.begin(), sorted_cells.end(), CellPlaceOrderLess(this));

  // Place multi-row instances first.
  if (have_multi_row_cells_) {
    for (Cell* cell : sorted_cells) {
      if (isMultiRow(cell) && cellFitsInCore(cell)) {
        if (!mapMove(cell)) {
          shiftMove(cell);
        }
      }
    }
  }
  for (Cell* cell : sorted_cells) {
    if (!isMultiRow(cell) && cellFitsInCore(cell)) {
      debugPrint(logger_,
                 DPL,
                 "place",
                 1,
                 "place {} {} {}",
                 cell->name(),
                 cell->x_,
                 cell->y_);
      if (!mapMove(cell)) {
        shiftMove(cell);
      }
    } else if (!isMultiRow(cell)) {
      debugPrint(logger_,
                 DPL,
                 "place",
                 1,
                 "skip place {} {} {}",
                 cell->name(),
                 cell->x_,
                 cell->y_);
    }
  }
  // This has negligible benefit -cherry
  // anneal();
}

bool Opendp::cellFitsInCore(Cell* cell)
{
  return gridPaddedWidth(cell) <= row_site_count_
         && gridHeight(cell) <= row_count_;
}

void Opendp::placeGroups2()
{
  for (Group& group : groups_) {
    vector<Cell*> group_cells;
    group_cells.reserve(cells_.size());
    for (Cell* cell : group.cells_) {
      if (!isFixed(cell) && !cell->is_placed_) {
        group_cells.push_back(cell);
      }
    }
    sort(group_cells.begin(), group_cells.end(), CellPlaceOrderLess(this));

    // Place multi-row cells in each group region.
    bool multi_pass = true;
    for (Cell* cell : group_cells) {
      if (!isFixed(cell) && !cell->is_placed_) {
        assert(cell->inGroup());
        if (isMultiRow(cell)) {
          multi_pass = mapMove(cell);
          if (!multi_pass) {
            break;
          }
        }
      }
    }
    bool single_pass = true;
    if (multi_pass) {
      // Place single-row cells in each group region.
      for (Cell* cell : group_cells) {
        if (!isFixed(cell) && !cell->is_placed_) {
          assert(cell->inGroup());
          if (!isMultiRow(cell)) {
            debugPrint(
                logger_, DPL, "place", 4, "place groups 2 {}", cell->name());
            single_pass = mapMove(cell);
            if (!single_pass) {
              break;
            }
          }
        }
      }
    }

    if (!single_pass || !multi_pass) {
      // Erase group cells
      for (Cell* cell : group.cells_) {
        erasePixel(cell);
      }

      // Determine brick placement by utilization.
      // magic number alert
      if (group.util > 0.95) {
        brickPlace1(&group);
      } else {
        brickPlace2(&group);
      }
    }
  }
}

// Place cells in group toward edges.
void Opendp::brickPlace1(const Group* group)
{
  const Rect* boundary = &group->boundary;
  vector<Cell*> sorted_cells(group->cells_);

  sort(sorted_cells.begin(), sorted_cells.end(), [&](Cell* cell1, Cell* cell2) {
    return rectDist(cell1, boundary) < rectDist(cell2, boundary);
  });

  for (Cell* cell : sorted_cells) {
    int x, y;
    rectDist(cell, boundary, &x, &y);
    Point legal = legalGridPt(cell, Point(x, y));
    // This looks for a site starting at the nearest corner in rect,
    // which seems broken. It should start looking at the nearest point
    // on the rect boundary. -cherry
    debugPrint(logger_,
               DPL,
               "place",
               4,
               "brick place {} {} {}",
               cell->name(),
               legal.getX(),
               legal.getY());
    if (!mapMove(cell, legal)) {
      logger_->error(DPL, 16, "cannot place instance {}.", cell->name());
    }
  }
}

void Opendp::rectDist(const Cell* cell,
                      const Rect* rect,
                      // Return values.
                      int* x,
                      int* y) const
{
  Point init = initialLocation(cell, false);
  int init_x = init.getX();
  int init_y = init.getY();

  if (init_x > (rect->xMin() + rect->xMax()) / 2) {
    *x = rect->xMax();
  } else {
    *x = rect->xMin();
  }

  if (init_y > (rect->yMin() + rect->yMax()) / 2) {
    *y = rect->yMax();
  } else {
    *y = rect->yMin();
  }
}

int Opendp::rectDist(const Cell* cell, const Rect* rect) const
{
  int x, y;
  rectDist(cell, rect, &x, &y);
  Point init = initialLocation(cell, false);
  return abs(init.getX() - x) + abs(init.getY() - y);
}

// Place group cells toward region edges.
void Opendp::brickPlace2(const Group* group)
{
  vector<Cell*> sorted_cells(group->cells_);

  sort(sorted_cells.begin(), sorted_cells.end(), [&](Cell* cell1, Cell* cell2) {
    return rectDist(cell1, cell1->region_) < rectDist(cell2, cell2->region_);
  });

  for (Cell* cell : sorted_cells) {
    if (!cell->hold_) {
      int x, y;
      rectDist(cell, cell->region_, &x, &y);
      Point legal = legalGridPt(cell, Point(x, y));
      // This looks for a site starting at the nearest corner in rect,
      // which seems broken. It should start looking at the nearest point
      // on the rect boundary. -cherry
      debugPrint(logger_,
                 DPL,
                 "place",
                 4,
                 "brickplace 2 {} {} {}",
                 cell->name(),
                 legal.getX(),
                 legal.getY());
      if (!mapMove(cell, legal)) {
        logger_->error(DPL, 17, "cannot place instance {}.", cell->name());
      }
    }
  }
}

int Opendp::groupRefine(const Group* group)
{
  vector<Cell*> sort_by_disp(group->cells_);

  sort(sort_by_disp.begin(), sort_by_disp.end(), [&](Cell* cell1, Cell* cell2) {
    return (disp(cell1) > disp(cell2));
  });

  int count = 0;
  for (int i = 0; i < sort_by_disp.size() * group_refine_percent_; i++) {
    Cell* cell = sort_by_disp[i];
    if (!cell->hold_) {
      if (refineMove(cell)) {
        count++;
      }
    }
  }
  return count;
}

// This is NOT annealing. It is random swapping. -cherry
int Opendp::anneal(Group* group)
{
  srand(rand_seed_);
  int count = 0;

  // magic number alert
  for (int i = 0; i < 100 * group->cells_.size(); i++) {
    Cell* cell1 = group->cells_[rand() % group->cells_.size()];
    Cell* cell2 = group->cells_[rand() % group->cells_.size()];
    if (swapCells(cell1, cell2)) {
      count++;
    }
  }
  return count;
}

// This is NOT annealing. It is random swapping. -cherry
int Opendp::anneal()
{
  srand(rand_seed_);
  int count = 0;
  // magic number alert
  for (int i = 0; i < 100 * cells_.size(); i++) {
    Cell* cell1 = &cells_[rand() % cells_.size()];
    Cell* cell2 = &cells_[rand() % cells_.size()];
    if (swapCells(cell1, cell2)) {
      count++;
    }
  }
  return count;
}

// Not called -cherry.
int Opendp::refine()
{
  vector<Cell*> sorted;
  sorted.reserve(cells_.size());

  for (Cell& cell : cells_) {
    if (!(isFixed(&cell) || cell.hold_ || cell.inGroup())) {
      sorted.push_back(&cell);
    }
  }
  sort(sorted.begin(), sorted.end(), [&](Cell* cell1, Cell* cell2) {
    return disp(cell1) > disp(cell2);
  });

  int count = 0;
  for (int i = 0; i < sorted.size() * refine_percent_; i++) {
    Cell* cell = sorted[i];
    if (!cell->hold_) {
      if (refineMove(cell)) {
        count++;
      }
    }
  }
  return count;
}

////////////////////////////////////////////////////////////////

bool Opendp::mapMove(Cell* cell)
{
  Point init = legalGridPt(cell, true);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "initial map move {} {} {}.",
             cell->name(),
             init.getX(),
             init.getY());
  return mapMove(cell, init);
}

bool Opendp::mapMove(Cell* cell, const Point& grid_pt)
{
  int grid_x = grid_pt.getX();
  int grid_y = grid_pt.getY();
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "mapMove {} {} {}.",
             cell->name(),
             grid_x,
             grid_y);
  PixelPt pixel_pt = diamondSearch(cell, grid_x, grid_y);
  if (pixel_pt.pixel) {
    paintPixel(cell, pixel_pt.pt.getX(), pixel_pt.pt.getY());
    if (debug_observer_) {
      debug_observer_->placeInstance(cell->db_inst_);
    }
    return true;
  }
  return false;
}

void Opendp::shiftMove(Cell* cell)
{
  debugPrint(logger_, DPL, "place", 1, "shift move {}.", cell->name());
  Point grid_pt = legalGridPt(cell, true);
  int grid_x = grid_pt.getX();
  int grid_y = grid_pt.getY();
  int row_height = getRowHeight(cell);
  int site_width = getSiteWidth(cell);
  int grid_index = grid_info_map_[row_height].grid_index;
  // magic number alert
  int boundary_margin = 3;
  int margin_width = gridPaddedWidth(cell, site_width) * boundary_margin;
  set<Cell*> region_cells;
  for (int x = grid_x - margin_width; x < grid_x + margin_width; x++) {
    for (int y = grid_y - boundary_margin; y < grid_y + boundary_margin; y++) {
      Pixel* pixel = gridPixel(grid_index, x, y);
      if (pixel) {
        Cell* cell = pixel->cell;
        if (cell && !isFixed(cell)) {
          region_cells.insert(cell);
        }
      }
    }
  }

  // erase region cells
  for (Cell* around_cell : region_cells) {
    if (cell->inGroup() == around_cell->inGroup()) {
      erasePixel(around_cell);
    }
  }

  // place target cell
  debugPrint(logger_, DPL, "place", 4, "shift move {}", cell->name());
  if (!mapMove(cell)) {
    placement_failures_.push_back(cell->db_inst_);
  }

  // re-place erased cells
  for (Cell* around_cell : region_cells) {
    debugPrint(logger_, DPL, "place", 4, "re-place {}", cell->name());
    if (cell->inGroup() == around_cell->inGroup() && !mapMove(around_cell)) {
      placement_failures_.push_back(cell->db_inst_);
    }
  }
}

bool Opendp::swapCells(Cell* cell1, Cell* cell2)
{
  if (cell1 != cell2 && !cell1->hold_ && !cell2->hold_
      && cell1->width_ == cell2->width_ && cell1->height_ == cell2->height_
      && !isFixed(cell1) && !isFixed(cell2)) {
    int dist_change = distChange(cell1, cell2->x_, cell2->y_)
                      + distChange(cell2, cell1->x_, cell1->y_);

    if (dist_change < 0) {
      int grid_x1 = gridPaddedX(cell2);
      int grid_y1 = gridY(cell2);
      int grid_x2 = gridPaddedX(cell1);
      int grid_y2 = gridY(cell1);

      erasePixel(cell1);
      erasePixel(cell2);
      paintPixel(cell1, grid_x1, grid_y1);
      paintPixel(cell2, grid_x2, grid_y2);
      return true;
    }
  }
  return false;
}

bool Opendp::refineMove(Cell* cell)
{
  int row_height = getRowHeight(cell);
  int site_width = getSiteWidth(cell);
  Point grid_pt = legalGridPt(cell, true, row_height, site_width);
  int grid_x = grid_pt.getX();
  int grid_y = grid_pt.getY();
  PixelPt pixel_pt = diamondSearch(cell, grid_x, grid_y);

  if (pixel_pt.pixel) {
    int scaled_max_displacement_y_
        = map_coordinates(max_displacement_y_, row_height_, getRowHeight(cell));
    if (abs(grid_x - pixel_pt.pt.getX()) > max_displacement_x_
        || abs(grid_y - pixel_pt.pt.getY()) > scaled_max_displacement_y_) {
      return false;
    }

    int dist_change = distChange(
        cell, pixel_pt.pt.getX() * site_width, pixel_pt.pt.getY() * row_height);

    if (dist_change < 0) {
      erasePixel(cell);
      paintPixel(cell, pixel_pt.pt.getX(), pixel_pt.pt.getY());
      return true;
    }
  }
  return false;
}

int Opendp::distChange(const Cell* cell, int x, int y) const
{
  Point init = initialLocation(cell, false);
  int init_x = init.getX();
  int init_y = init.getY();
  int cell_dist = abs(cell->x_ - init_x) + abs(cell->y_ - init_y);
  int pt_dist = abs(init_x - x) + abs(init_y - y);
  return pt_dist - cell_dist;
}

////////////////////////////////////////////////////////////////

PixelPt Opendp::diamondSearch(const Cell* cell,
                              // grid
                              int x,
                              int y) const
{
  // Diamond search limits.
  int x_min = x - max_displacement_x_;
  int x_max = x + max_displacement_x_;
  // TODO: IMO, this is still not correct.
  //  I am scaling based on the smallest row_height to keep code consistent with
  //  the original code.
  //  max_displacement_y_ is in microns, and this doesn't translate directly to
  //  x and y on the grid.
  int scaled_max_displacement_y_
      = map_coordinates(max_displacement_y_, row_height_, getRowHeight(cell));
  int y_min = y - scaled_max_displacement_y_;
  int y_max = y + scaled_max_displacement_y_;

  auto [row_height, grid_info] = getRowInfo(cell);
  int site_width = getSiteWidth(cell);

  // Restrict search to group boundary.
  Group* group = cell->group_;
  if (group) {
    // Map boundary to grid staying inside.
    Rect grid_boundary(divCeil(group->boundary.xMin(), site_width),
                       divCeil(group->boundary.yMin(), row_height),
                       group->boundary.xMax() / site_width,
                       group->boundary.yMax() / row_height);
    Point min = grid_boundary.closestPtInside(Point(x_min, y_min));
    Point max = grid_boundary.closestPtInside(Point(x_max, y_max));
    x_min = min.getX();
    y_min = min.getY();
    x_max = max.getX();
    y_max = max.getY();
  }

  // Clip diamond limits to grid bounds.
  x_min = max(0, x_min);
  y_min = max(0, y_min);
  x_max = min(grid_info.site_count, x_max);
  y_max = min(grid_info.row_count, y_max);
  debugPrint(logger_,
             DPL,
             "group",
             1,
             "x_min {} x_max {} y_min {} y_max {}",
             x_min,
             x_max,
             y_min,
             y_max);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "Diamond Search {} ({}, {}) bounds ({}-{}, {}-{})",
             cell->name(),
             x,
             y,
             x_min,
             x_max - 1,
             y_min,
             y_max - 1);

  // Check the bin at the initial position first.
  PixelPt avail_pt = binSearch(x, cell, x, y);
  if (avail_pt.pixel) {
    return avail_pt;
  }

  for (int i = 1; i < std::max(scaled_max_displacement_y_, max_displacement_x_);
       i++) {
    PixelPt best_pt;
    int best_dist = 0;
    // left side
    for (int j = 1; j < i * 2; j++) {
      int x_offset = -((j + 1) / 2);
      int y_offset = (i * 2 - j) / 2;
      if (abs(x_offset) < max_displacement_x_
          && abs(y_offset) < scaled_max_displacement_y_) {
        if (j % 2 == 1) {
          y_offset = -y_offset;
        }
        diamondSearchSide(cell,
                          x,
                          y,
                          x_min,
                          y_min,
                          x_max,
                          y_max,
                          x_offset,
                          y_offset,
                          best_pt,
                          best_dist);
      }
    }

    // right side
    for (int j = 1; j < (i + 1) * 2; j++) {
      int x_offset = (j - 1) / 2;
      int y_offset = ((i + 1) * 2 - j) / 2;
      if (abs(x_offset) < max_displacement_x_
          && abs(y_offset) < scaled_max_displacement_y_) {
        if (j % 2 == 1) {
          y_offset = -y_offset;
        }
        diamondSearchSide(cell,
                          x,
                          y,
                          x_min,
                          y_min,
                          x_max,
                          y_max,
                          x_offset,
                          y_offset,
                          best_pt,
                          best_dist);
      }
    }
    if (best_pt.pixel) {
      return best_pt;
    }
  }
  return PixelPt();
}

void Opendp::diamondSearchSide(const Cell* cell,
                               int x,
                               int y,
                               int x_min,
                               int y_min,
                               int x_max,
                               int y_max,
                               int x_offset,
                               int y_offset,
                               // Return values
                               PixelPt& best_pt,
                               int& best_dist) const
{
  int bin_x = min(x_max, max(x_min, x + x_offset * bin_search_width_));
  int bin_y = min(y_max, max(y_min, y + y_offset));
  PixelPt avail_pt = binSearch(x, cell, bin_x, bin_y);
  if (avail_pt.pixel) {
    int avail_dist = abs(x - avail_pt.pt.getX()) * getSiteWidth(cell)
                     + abs(y - avail_pt.pt.getY()) * getRowHeight(cell);
    if (best_pt.pixel == nullptr || avail_dist < best_dist) {
      best_pt = avail_pt;
      best_dist = avail_dist;
    }
  }
}

PixelPt Opendp::binSearch(int x, const Cell* cell, int bin_x, int bin_y) const
{
  debugPrint(logger_,
             DPL,
             "place",
             3,
             " Bin Search {} ({:4} {}> {:4},{:4})",
             cell->name(),
             x > bin_x ? bin_x + bin_search_width_ - 1 : bin_x,
             x > bin_x ? "-" : "+",
             x > bin_x ? bin_x : bin_x + bin_search_width_ - 1,
             bin_y);

  int x_end = bin_x + gridPaddedWidth(cell);
  int row_height = getRowHeight(cell);
  int height = gridHeight(cell, row_height);
  int y_end = bin_y + height;
  auto grid_info = grid_info_map_.at(row_height);
  if (debug_observer_) {
    debug_observer_->binSearch(cell, bin_x, bin_y, x_end, y_end);
  }

  if (y_end > grid_info.row_count) {
    return PixelPt();
  }

  if (x > bin_x) {
    for (int i = bin_search_width_ - 1; i >= 0; i--) {
      if (checkPixels(cell, bin_x + i, bin_y, x_end + i, y_end)) {
        return PixelPt(gridPixel(grid_info.grid_index, bin_x + i, bin_y),
                       bin_x + i,
                       bin_y);
      }
    }
  } else {
    for (int i = 0; i < bin_search_width_; i++) {
      if (checkPixels(cell, bin_x + i, bin_y, x_end + i, y_end)) {
        return PixelPt(gridPixel(grid_info.grid_index, bin_x + i, bin_y),
                       bin_x + i,
                       bin_y);
      }
    }
  }
  return PixelPt();
}

// Check all pixels are empty.
bool Opendp::checkPixels(const Cell* cell,
                         int x,
                         int y,
                         int x_end,
                         int y_end) const
{
  auto row_info = getRowInfo(cell);
  if (x_end > row_info.second.site_count) {
    return false;
  }

  int layer = row_info.second.grid_index;
  for (int y1 = y; y1 < y_end; y1++) {
    for (int x1 = x; x1 < x_end; x1++) {
      Pixel* pixel = gridPixel(layer, x1, y1);
      if (pixel == nullptr || pixel->cell || !pixel->is_valid
          || (cell->inGroup() && pixel->group_ != cell->group_)
          || (!cell->inGroup() && pixel->group_)) {
        return false;
      }
    }
    if (disallow_one_site_gaps_) {
      // here we need to check for abutting first, if there is an abutting cell
      // then we continue as there is nothing wrong with it
      // if there is no abutting cell, we will then check cells at 1+ distances
      // we only need to check on the left and right sides
      int x_begin = max(0, x - 1);
      int y_begin = max(0, y - 1);
      // inclusive search, so we don't add 1 to the end
      int x_finish = min(x_end, row_site_count_ - 1);
      int y_finish = min(y_end, row_count_ - 1);
      auto isAbutted = [this](int layer, int x, int y) {
        Pixel* pixel = gridPixel(layer, x, y);
        return (pixel == nullptr || pixel->cell);
      };
      auto cellAtSite = [this](int layer, int x, int y) {
        Pixel* pixel = gridPixel(layer, x, y);
        return (pixel != nullptr && pixel->cell);
      };
      // upper left corner
      if (!isAbutted(layer, x_begin, y_begin)
          && cellAtSite(layer, x_begin - 1, y_begin)) {
        return false;
      }
      // lower left corner
      if (!isAbutted(layer, x_begin, y_finish)
          && cellAtSite(layer, x_begin - 1, y_finish)) {
        return false;
      }
      // upper right corner
      if (!isAbutted(layer, x_finish, y_begin)
          && cellAtSite(layer, x_finish + 1, y_begin)) {
        return false;
      }
      // lower right corner
      if (!isAbutted(layer, x_finish, y_finish)
          && cellAtSite(layer, x_finish + 1, y_finish)) {
        return false;
      }
    }
  }
  return true;
}

////////////////////////////////////////////////////////////////

// Legalize cell origin
//  inside the core
//  row site

Point Opendp::legalPt(const Cell* cell,
                      const Point& pt,
                      int row_height,
                      int site_width) const
{
  // Move inside core.

  if (row_height == -1) {
    row_height = getRowHeight(cell);
  }
  auto grid_info = grid_info_map_.at(row_height);
  if (site_width == -1) {
    site_width = getSiteWidth(cell);
  }
  int core_x = min(max(0, pt.getX()),
                   grid_info.site_count * site_width - cell->width_);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "core_x {} {} {}",
             core_x,
             grid_info.site_count,
             site_width);
  int core_y = min(max(0, pt.getY()),
                   grid_info.row_count * row_height - cell->height_);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "core_y {} {} {}",
             core_y,
             grid_info.row_count,
             row_height);
  // Align with row site.
  int grid_x = divRound(core_x, site_width);
  int grid_y = divRound(core_y, row_height);

  int legal_x = grid_x * site_width;
  int legal_y = grid_y * row_height;
  debugPrint(logger_, DPL, "place", 1, "legalPt {} {}", legal_x, legal_y);
  return Point(legal_x, legal_y);
}

Point Opendp::legalGridPt(const Cell* cell,
                          const Point& pt,
                          int row_height,
                          int site_width) const
{
  if (site_width == -1) {
    site_width = getSiteWidth(cell);
  }
  if (row_height == -1) {
    row_height = getRowHeight(cell);
  }
  debugPrint(
      logger_, DPL, "place", 1, "legalGridPt {} {}", pt.getX(), pt.getY());
  Point legal = legalPt(cell, pt, row_height, site_width);
  return Point(gridX(legal.getX(), site_width),
               gridY(legal.getY(), row_height));
}

Point Opendp::nearestBlockEdge(const Cell* cell,
                               const Point& legal_pt,
                               const Rect& block_bbox) const
{
  const int legal_x = legal_pt.getX();
  const int legal_y = legal_pt.getY();
  const int row_height = getRowHeight(cell);
  const int site_width = getSiteWidth(cell);
  const int x_min_dist = abs(legal_x - block_bbox.xMin());
  const int x_max_dist = abs(block_bbox.xMax() - (legal_x + cell->width_));
  const int y_min_dist = abs(legal_y - block_bbox.yMin());
  const int y_max_dist = abs(block_bbox.yMax() - (legal_y + cell->height_));
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "nearestBlockEdge {} {} {} {} {} {}",
             legal_x,
             legal_y,
             block_bbox.xMin(),
             block_bbox.xMax(),
             block_bbox.yMin(),
             block_bbox.yMax());
  if (x_min_dist < x_max_dist && x_min_dist < y_min_dist
      && x_min_dist < y_max_dist) {
    // left of block
    return legalPt(cell,
                   Point(block_bbox.xMin() - cell->width_, legal_pt.getY()),
                   row_height,
                   site_width);
  }
  if (x_max_dist <= x_min_dist && x_max_dist <= y_min_dist
      && x_max_dist <= y_max_dist) {
    // right of block
    return legalPt(cell,
                   Point(block_bbox.xMax(), legal_pt.getY()),
                   row_height,
                   site_width);
  }
  if (y_min_dist <= x_min_dist && y_min_dist <= x_max_dist
      && y_min_dist <= y_max_dist) {
    // below block
    return legalPt(cell,
                   Point(legal_pt.getX(),
                         divFloor(block_bbox.yMin(), row_height) * row_height
                             - cell->height_),
                   row_height,
                   site_width);
  }
  // above block
  return legalPt(cell,
                 Point(legal_pt.getX(),
                       divCeil(block_bbox.yMax(), row_height) * row_height),
                 row_height,
                 site_width);
}

// Find the nearest valid site left/right/above/below, if any.
// The site doesn't need to be empty but mearly valid.  That should
// be a reasonable place to start the search.  Returns true if any
// site can be found.
bool Opendp::moveHopeless(const Cell* cell, int& grid_x, int& grid_y) const
{
  int best_x = grid_x;
  int best_y = grid_y;
  int best_dist = std::numeric_limits<int>::max();
  int site_width = getSiteWidth(cell);
  auto [row_height, grid_info] = getRowInfo(cell);
  int grid_index = grid_info.grid_index;
  int layer_site_count = divFloor(core_.dx(), site_width);
  int layer_row_count = divFloor(core_.dy(), row_height);

  // since the site doesn't have to be empty, we don't need to check all layers.
  // They will be checked in the checkPixels in the diamondSearch method after
  // this initialization
  for (int x = grid_x - 1; x >= 0; --x) {  // left
    if (grid_[grid_index][grid_y][x].is_valid) {
      best_dist = (grid_x - x - 1) * site_width;
      best_x = x;
      best_y = grid_y;
      break;
    }
  }
  for (int x = grid_x + 1; x < layer_site_count; ++x) {  // right
    if (grid_[grid_index][grid_y][x].is_valid) {
      const int dist = (x - grid_x) * site_width - cell->width_;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = x;
        best_y = grid_y;
      }
      break;
    }
  }
  for (int y = grid_y - 1; y >= 0; --y) {  // below
    if (grid_[grid_index][y][grid_x].is_valid) {
      const int dist = (grid_y - y - 1) * row_height;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = grid_x;
        best_y = y;
      }
      break;
    }
  }
  for (int y = grid_y + 1; y < layer_row_count; ++y) {  // above
    if (grid_[grid_index][y][grid_x].is_valid) {
      const int dist = (y - grid_y) * row_height - cell->height_;
      if (dist < best_dist) {
        best_dist = dist;
        best_x = grid_x;
        best_y = y;
      }
      break;
    }
  }
  if (best_dist != std::numeric_limits<int>::max()) {
    grid_x = best_x;
    grid_y = best_y;
    return true;
  }
  return false;
}

// Legalize pt origin for cell
//  inside the core
//  row site
//  not on top of a macro
//  not in a hopeless site
Point Opendp::legalPt(const Cell* cell,
                      bool padded,
                      int row_height,
                      int site_width) const
{
  if (isFixed(cell)) {
    logger_->critical(DPL, 26, "legalPt called on fixed cell.");
  }

  if (row_height == -1) {
    row_height = getRowHeight(cell);
  }
  if (site_width == -1) {
    site_width = getSiteWidth(cell);
  }

  Point init = initialLocation(cell, padded);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "legalpt itself init {} {} ",
             init.getX(),
             init.getY());
  Point legal_pt = legalPt(cell, init, row_height, site_width);
  auto grid_info = getGridInfo(cell);
  int grid_x = gridX(legal_pt.getX(), site_width);
  int grid_y = gridY(legal_pt.getY(), row_height);
  debugPrint(logger_, DPL, "place", 1, "grid_x {} grid_y {}", grid_x, grid_y);

  Pixel* pixel = gridPixel(grid_info.grid_index, grid_x, grid_y);
  if (pixel) {
    // Move std cells off of macros.  First try the is_hopeless strategy
    debugPrint(logger_,
               DPL,
               "hopeless",
               1,
               "is pixel {} , {} , {} hopeless? {}",
               grid_info.grid_index,
               grid_x,
               grid_y,
               pixel->is_hopeless ? " true " : " false ");
    if (pixel->is_hopeless && moveHopeless(cell, grid_x, grid_y)) {
      legal_pt = Point(grid_x * site_width, grid_y * row_height);
      pixel = gridPixel(grid_info.grid_index, grid_x, grid_y);
      debugPrint(logger_,
                 DPL,
                 "place",
                 2,
                 "legalpt hopeless {} {} ",
                 legal_pt.getX(),
                 legal_pt.getY());
    }

    const Cell* block = pixel->cell;

    // If that didn't do the job fall back on the old move to nearest
    // edge strategy.  This doesn't consider site availability at the
    // end used so it is secondary.
    if (block && isBlock(block)) {
      debugPrint(logger_,
                 DPL,
                 "place",
                 2,
                 "legalpt block {} {} {} ",
                 block->x_,
                 block->y_,
                 block->width_);
      const Rect block_bbox(block->x_,
                            block->y_,
                            block->x_ + block->width_,
                            block->y_ + block->height_);
      const int legal_x = legal_pt.getX();
      const int legal_y = legal_pt.getY();
      debugPrint(logger_,
                 DPL,
                 "place",
                 2,
                 "legalpt blockbbox {} {} {} {} {} {} ",
                 legal_x,
                 legal_y,
                 block_bbox.xMin(),
                 block_bbox.xMax(),
                 block_bbox.yMin(),
                 block_bbox.yMax());
      if ((legal_x + cell->width_) >= block_bbox.xMin()
          && legal_x <= block_bbox.xMax()
          && (legal_y + cell->height_) >= block_bbox.yMin()
          && legal_y <= block_bbox.yMax()) {
        legal_pt = nearestBlockEdge(cell, legal_pt, block_bbox);
      }
    } else {
      debugPrint(logger_,
                 DPL,
                 "place",
                 2,
                 "legalpt no block {} {} {} ",
                 legal_pt.getX(),
                 legal_pt.getY(),
                 cell->width_);
    }
  } else {
    debugPrint(logger_,
               DPL,
               "place",
               2,
               "legalpt no pixel {} {} {} ",
               legal_pt.getX(),
               legal_pt.getY(),
               cell->width_);
  }

  return legal_pt;
}

Point Opendp::legalGridPt(const Cell* cell,
                          bool padded,
                          int row_height,
                          int site_width) const
{
  if (site_width == -1) {
    site_width = getSiteWidth(cell);
  }
  if (row_height == -1) {
    row_height = getRowHeight(cell);
  }
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "legalgridpt bef {} {} {} {} {} {}",
             cell->name(),
             cell->x_,
             cell->y_,
             cell->width_,
             cell->height_,
             cell->orient_);
  Point pt = legalPt(cell, padded, row_height, site_width);
  debugPrint(logger_,
             DPL,
             "place",
             1,
             "legalpt 1 {} {} {} {} {} {}",
             cell->name(),
             pt.getX(),
             pt.getY(),
             cell->width_,
             cell->height_,
             cell->orient_);
  return Point(gridX(pt.getX(), site_width), gridY(pt.getY(), row_height));
}

////////////////////////////////////////////////////////////////

PixelPt::PixelPt(Pixel* pixel1, int grid_x, int grid_y)
    : pixel(pixel1), pt(grid_x, grid_y)
{
}

}  // namespace dpl
