/* Authors: Lutong Wang and Bangqi Xu */
/*
 * Copyright (c) 2019, The Regents of the University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _FR_TERM_H_
#define _FR_TERM_H_

#include <memory>

#include "db/obj/frBlockObject.h"
#include "db/obj/frNet.h"
#include "db/obj/frPin.h"
#include "frBaseTypes.h"

namespace fr {
class frNet;
class frInstTerm;
class frBlock;

class frTerm : public frBlockObject
{
 public:
  // constructors
  frTerm(const frString& name)
      : frBlockObject(),
        name_(name),
        block_(nullptr),
        net_(nullptr),
        pins_(),
        type_(dbSigType::SIGNAL),
        direction_(dbIoType::INPUT),
        bbox_()
  {
  }
  frTerm(const frTerm& in)
      : frBlockObject(),
        name_(in.name_),
        block_(in.block_),
        net_(in.net_),
        type_(in.type_),
        direction_(in.direction_),
        bbox_()
  {
    for (auto& uPin : in.getPins()) {
      auto pin = uPin.get();
      auto tmp = std::make_unique<frPin>(*pin);
      addPin(std::move(tmp));
    }
  }
  frTerm(const frTerm& in, const dbTransform& xform)
      : frBlockObject(),
        name_(in.name_),
        block_(in.block_),
        net_(in.net_),
        type_(in.type_),
        direction_(in.direction_),
        bbox_()
  {
    for (auto& uPin : in.getPins()) {
      auto pin = uPin.get();
      auto tmp = std::make_unique<frPin>(*pin, xform);
      addPin(std::move(tmp));
    }
  }
  // getters
  frBlock* getBlock() const { return block_; }
  bool hasNet() const { return (net_); }
  frNet* getNet() const { return net_; }
  const frString& getName() const { return name_; }
  const std::vector<std::unique_ptr<frPin>>& getPins() const { return pins_; }
  // setters
  void setBlock(frBlock* in) { block_ = in; }
  void addToNet(frNet* in) { net_ = in; }
  void addPin(std::unique_ptr<frPin> in)
  {
    in->setTerm(this);
    for (auto& uFig : in->getFigs()) {
      auto pinFig = uFig.get();
      if (pinFig->typeId() == frcRect) {
        if (bbox_.dx() == 0 && bbox_.dy() == 0)
          bbox_ = static_cast<frRect*>(pinFig)->getBBox();
        else
          bbox_.merge(static_cast<frRect*>(pinFig)->getBBox());
      }
    }
    pins_.push_back(std::move(in));
  }
  void setType(dbSigType in) { type_ = in; }
  dbSigType getType() const { return type_; }
  void setDirection(dbIoType in) { direction_ = in; }
  dbIoType getDirection() const { return direction_; }
  // others
  frBlockObjectEnum typeId() const override { return frcTerm; }
  void setOrderId(int order_id) { _order_id = order_id; }
  int getOrderId() { return _order_id; }
  frAccessPoint* getAccessPoint(frCoord x,
                                frCoord y,
                                frLayerNum lNum,
                                int pinAccessIdx)
  {
    if (pinAccessIdx == -1) {
      return nullptr;
    }
    for (auto& pin : pins_) {
      if (!pin->hasPinAccess()) {
        continue;
      }
      for (auto& ap : pin->getPinAccess(pinAccessIdx)->getAccessPoints()) {
        if (x == ap->getPoint().x() && y == ap->getPoint().y()
            && lNum == ap->getLayerNum()) {
          return ap.get();
        }
      }
    }
    return nullptr;
  }
  bool hasAccessPoint(frCoord x, frCoord y, frLayerNum lNum, int pinAccessIdx)
  {
    return getAccessPoint(x, y, lNum, pinAccessIdx) != nullptr;
  }
  // fills outShapes with copies of the pinFigs
  void getShapes(std::vector<frRect>& outShapes)
  {
    for (auto& pin : pins_) {
      for (auto& pinShape : pin->getFigs()) {
        if (pinShape->typeId() == frcRect) {
          outShapes.push_back(*static_cast<frRect*>(pinShape.get()));
        }
      }
    }
  }
  const Rect getBBox() const { return bbox_; }

 protected:
  frString name_;  // A, B, Z, VSS, VDD
  frBlock* block_;
  frNet* net_;  // set later, term in instTerm does not have net
  std::vector<std::unique_ptr<frPin>> pins_;  // set later
  dbSigType type_;
  dbIoType direction_;
  int _order_id;
  Rect bbox_;

  template <class Archive>
  void serialize(Archive& ar, const unsigned int version);

  frTerm() = default;  // for serialization

  friend class boost::serialization::access;
};

template <class Archive>
void frTerm::serialize(Archive& ar, const unsigned int version)
{
  (ar) & boost::serialization::base_object<frBlockObject>(*this);
  (ar) & name_;
  (ar) & block_;
  // (ar) & net_; handled by net
  (ar) & pins_;
  (ar) & type_;
  (ar) & direction_;
  (ar) & _order_id;
  (ar) & bbox_;
}
}  // namespace fr

#endif
