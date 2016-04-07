/*
 *  Copyright (C) 2015-2016, Mike Walters <mike@flomp.net>
 *
 *  This file is part of inspectrum.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "plotview.h"
#include <QApplication>
#include <QDebug>
#include <QPainter>
#include <QScrollBar>
#include <gnuradio/top_block.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/blocks/multiply_const_cc.h>
#include "grsamplebuffer.h"
#include "memory_sink.h"
#include "memory_source.h"

PlotView::PlotView(InputSource *input) : cursors(this), viewRange({0, 0})
{
    mainSampleSource = input;
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    enableCursors(false);
    connect(&cursors, SIGNAL(cursorsMoved()), this, SLOT(cursorsMoved()));

    spectrogramPlot = new SpectrogramPlot(std::shared_ptr<SampleSource<std::complex<float>>>(mainSampleSource));
    auto tunerOutput = std::dynamic_pointer_cast<SampleSource<std::complex<float>>>(spectrogramPlot->output()).get();
    iqPlot = createIQPlot(tunerOutput);
    auto quadDemodPlot = createQuadratureDemodPlot(tunerOutput);

    addPlot(spectrogramPlot);
    addPlot(iqPlot);
    addPlot(quadDemodPlot);

    viewport()->installEventFilter(this);
    mainSampleSource->subscribe(this);
}

void PlotView::addPlot(Plot *plot)
{
    plots.emplace_back(plot);
    connect(plot, &Plot::repaint, this, &PlotView::repaint);
}

TracePlot* PlotView::createIQPlot(SampleSource<std::complex<float>> *src)
{
    gr::top_block_sptr iq_tb = gr::make_top_block("multiply");
    auto iq_mem_source = gr::blocks::memory_source::make(8);
    auto iq_mem_sink = gr::blocks::memory_sink::make(8);
    auto multiply = gr::blocks::multiply_const_cc::make(20);

    iq_tb->connect(iq_mem_source, 0, multiply, 0);
    iq_tb->connect(multiply, 0, iq_mem_sink, 0);

    auto iq_src = std::make_shared<GRSampleBuffer<std::complex<float>, std::complex<float>>>(src, iq_tb, iq_mem_source, iq_mem_sink);
    return new TracePlot(iq_src);
}

TracePlot* PlotView::createQuadratureDemodPlot(SampleSource<std::complex<float>> *src)
{
    gr::top_block_sptr quad_demod_tb = gr::make_top_block("quad_demod");
    auto quad_demod_mem_source = gr::blocks::memory_source::make(8);
    auto quad_demod_mem_sink = gr::blocks::memory_sink::make(4);
    auto quad_demod = gr::analog::quadrature_demod_cf::make(5);
    quad_demod_tb->connect(quad_demod_mem_source, 0, quad_demod, 0);
    quad_demod_tb->connect(quad_demod, 0, quad_demod_mem_sink, 0);

    return new TracePlot(
        std::make_shared<GRSampleBuffer<std::complex<float>, float>>(
            dynamic_cast<SampleSource<std::complex<float>>*>(src), quad_demod_tb, quad_demod_mem_source, quad_demod_mem_sink
        )
    );
}

void PlotView::cursorsMoved()
{
    selectedSamples = {
        horizontalScrollBar()->value() + cursors.selection().minimum * samplesPerLine(),
        horizontalScrollBar()->value() + cursors.selection().maximum * samplesPerLine()
    };

    off_t sampleCount = selectedSamples.length();
    float selectionTime = sampleCount / (float)mainSampleSource->rate();
    emit timeSelectionChanged(selectionTime);
    viewport()->update();
}

void PlotView::enableCursors(bool enabled)
{
    cursorsEnabled = enabled;
    if (enabled) {
        int margin = viewport()->rect().width() / 3;
        cursors.setSelection({viewport()->rect().left() + margin, viewport()->rect().right() - margin});
    }
    viewport()->update();
}

bool PlotView::eventFilter(QObject * obj, QEvent *event)
{
    // Pass mouse events to individual plot objects
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {

        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

        if (cursors.mouseEvent(event->type(), *mouseEvent))
            return true;

        int plotY = -verticalScrollBar()->value();
        for (auto&& plot : plots) {
            bool result = plot->mouseEvent(
                event->type(),
                QMouseEvent(
                    event->type(),
                    QPoint(mouseEvent->pos().x(), mouseEvent->pos().y() - plotY),
                    mouseEvent->button(),
                    mouseEvent->buttons(),
                    QApplication::keyboardModifiers()
                )
            );
            if (result)
                return true;
            plotY += plot->height();
        }
    }

    // Handle wheel events for zooming
    if (event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = (QWheelEvent*)event;
        if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
            if (wheelEvent->angleDelta().y() > 0) {
                emit zoomIn();
            } else if (wheelEvent->angleDelta().y() < 0) {
                emit zoomOut();
            }
            return true;
        }
    }
    return false;
}

void PlotView::invalidateEvent()
{
    horizontalScrollBar()->setMinimum(0);
    horizontalScrollBar()->setMaximum(mainSampleSource->count());
}

void PlotView::repaint()
{
    viewport()->update();
}

void PlotView::setCursorSegments(int segments)
{
    cursors.setSegments(segments);
    cursorsMoved();
    viewport()->update();
}

void PlotView::setFFTAndZoom(int size, int zoom)
{
    // Set new FFT size
    fftSize = size;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setFFTSize(size);

    // Set new zoom level
    zoomLevel = zoom;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setZoomLevel(zoom);

    // Update horizontal (time) scrollbar
    horizontalScrollBar()->setSingleStep(size * 10 / zoomLevel);
    horizontalScrollBar()->setPageStep(size * 100 / zoomLevel);

    updateView(true);
}

void PlotView::setPowerMin(int power)
{
    powerMin = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMin(power);
    updateView();
}

void PlotView::setPowerMax(int power)
{
    powerMax = power;
    if (spectrogramPlot != nullptr)
        spectrogramPlot->setPowerMax(power);
    updateView();
}

void PlotView::paintEvent(QPaintEvent *event)
{
    if (mainSampleSource == nullptr) return;

    QRect rect = QRect(0, 0, width(), height());
    QPainter painter(viewport());
    painter.fillRect(rect, Qt::black);


#define PLOT_LAYER(paintFunc)                                                   \
    {                                                                           \
        int y = -verticalScrollBar()->value();                                  \
        for (auto&& plot : plots) {                                             \
            QRect rect = QRect(0, y, width(), plot->height());                  \
            plot->paintFunc(painter, rect, viewRange);                          \
            y += plot->height();                                                \
        }                                                                       \
    }

    PLOT_LAYER(paintBack);
    PLOT_LAYER(paintMid);
    PLOT_LAYER(paintFront);
    if (cursorsEnabled)
        cursors.paintFront(painter, rect, viewRange);

#undef PLOT_LAYER
}

int PlotView::plotsHeight()
{
    int height = 0;
    for (auto&& plot : plots) {
        height += plot->height();
    }
    return height;
}

void PlotView::resizeEvent(QResizeEvent * event)
{
    updateView();
}

off_t PlotView::samplesPerLine()
{
    return fftSize / zoomLevel;
}

void PlotView::scrollContentsBy(int dx, int dy)
{
    updateView();
}

void PlotView::updateView(bool reCenter)
{
    // Store old view for recentering
    auto oldViewRange = viewRange;

    // Update current view
    viewRange = {
        horizontalScrollBar()->value(),
        horizontalScrollBar()->value() + width() * samplesPerLine()
    };

    // Adjust time offset to zoom around central sample
    if (reCenter) {
        horizontalScrollBar()->setValue(
            horizontalScrollBar()->value() + (oldViewRange.length() - viewRange.length()) / 2
        );
    }

    horizontalScrollBar()->setMaximum(mainSampleSource->count() - ((width() - 1) * samplesPerLine()));

    verticalScrollBar()->setMaximum(std::max(0, plotsHeight() - viewport()->height()));

    // Update cursors
    QRect rect = viewport()->rect();
    range_t<int> newSelection = {
        (int)((selectedSamples.minimum - horizontalScrollBar()->value()) / samplesPerLine()),
        (int)((selectedSamples.maximum - horizontalScrollBar()->value()) / samplesPerLine())
    };
    cursors.setSelection(newSelection);

    // Re-paint
    viewport()->update();
}
