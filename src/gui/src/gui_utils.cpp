/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2020, The Regents of the University of California
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
//
///////////////////////////////////////////////////////////////////////////////

#include "gui_utils.h"

#include <QFileDialog>
#include <QImageWriter>

#include "utl/Logger.h"

namespace gui {

QString Utils::requestImageSavePath(QWidget* parent, const QString& title)
{
  QList<QByteArray> valid_extensions = QImageWriter::supportedImageFormats();

  QString images_filter = "Images (";
  for (const QByteArray& ext : valid_extensions) {
    images_filter += "*." + ext + " ";
  }
  images_filter += ")";

  return QFileDialog::getSaveFileName(parent, title, "", images_filter);
}

QString Utils::fixImagePath(const QString& path, utl::Logger* logger)
{
  QList<QByteArray> valid_extensions = QImageWriter::supportedImageFormats();

  QString fixed_path = path;

  if (!std::any_of(
          valid_extensions.begin(),
          valid_extensions.end(),
          [path](const QString& ext) { return path.endsWith("." + ext); })) {
    fixed_path += ".png";
    if (logger != nullptr) {
      logger->warn(
          utl::GUI,
          10,
          "File path does not end with a valid extension, new path is: {}",
          fixed_path.toStdString());
    }
  }

  return fixed_path;
}

void Utils::renderImage(const QString& path,
                        QWidget* widget,
                        int width_px,
                        int height_px,
                        const QRect& render_rect,
                        const QColor& background,
                        utl::Logger* logger)
{
  QImage img(width_px, height_px, QImage::Format_ARGB32_Premultiplied);
  if (!img.isNull()) {
    img.fill(background);

    widget->render(&img, {0, 0}, render_rect);
    if (!img.save(path) && logger != nullptr) {
      logger->warn(
          utl::GUI, 11, "Failed to write image: {}", path.toStdString());
    }
  } else {
    if (logger != nullptr) {
      logger->warn(utl::GUI,
                   12,
                   "Image is too big to be generated: {}px x {}px",
                   width_px,
                   height_px);
    }
  }
}

}  // namespace gui
