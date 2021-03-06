/******************************************************************************

  This source file is part of the tomviz project.

  Copyright Kitware, Inc.

  This source code is released under the New BSD License, (the "License").

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

******************************************************************************/
#include "Pipeline.h"
#include "DataSource.h"
#include "Operator.h"
#include "PipelineWorker.h"

#include <QObject>
#include <QTimer>
#include <vtkTrivialProducer.h>

namespace tomviz {

Pipeline::Pipeline(DataSource* dataSource, QObject* parent)
  : QObject(parent)
{
  m_data = dataSource;
  m_worker = new PipelineWorker(this);
  m_data->setParent(this);

  addDataSource(dataSource);
}

Pipeline::~Pipeline() = default;

void Pipeline::execute()
{
  emit started();
  executePipelineBranch(m_data);
}

void Pipeline::execute(DataSource* start, bool last)
{
  emit started();
  Operator* lastOp = nullptr;

  if (last) {
    lastOp = start->operators().last();
  }
  executePipelineBranch(start, lastOp);
}

void Pipeline::execute(DataSource* start)
{
  emit started();
  executePipelineBranch(start);
}

void Pipeline::executePipelineBranch(DataSource* dataSource, Operator* start)
{
  if (m_paused) {
    return;
  }

  auto operators = dataSource->operators();
  if (operators.isEmpty()) {
    return;
  }

  // Cancel any running operators. TODO in the future we should be able to add
  // operators to end of a running pipeline.
  if (m_future && m_future->isRunning()) {
    m_future->cancel();
  }

  vtkDataObject* data = nullptr;

  if (start != nullptr) {
    // Use the transform DataSource as the starting point, if we have one.
    auto transformDataSource = findTransformedDataSource(dataSource);
    if (transformDataSource) {
      data = transformDataSource->copyData();
    }

    // See if we have any canceled operators in the pipeline, if so we have to
    // re-run this branch of the pipeline.
    bool haveCanceled = false;
    for (auto itr = dataSource->operators().begin(); *itr != start; ++itr) {
      auto currentOp = *itr;
      if (currentOp->isCanceled()) {
        currentOp->resetState();
        haveCanceled = true;
        break;
      }
    }

    // If we have canceled operators we have to run call operators.
    if (!haveCanceled) {
      operators = operators.mid(operators.indexOf(start));
      start->resetState();
    }
  }

  // Use the original
  if (data == nullptr) {
    data = dataSource->copyData();
  }

  m_future = m_worker->run(data, operators);
  connect(m_future, &PipelineWorker::Future::finished, this,
          &Pipeline::pipelineBranchFinished);
  connect(m_future, &PipelineWorker::Future::canceled, this,
          &Pipeline::pipelineBranchCanceled);
}

void Pipeline::pipelineBranchFinished(bool result)
{
  PipelineWorker::Future* future =
    qobject_cast<PipelineWorker::Future*>(sender());
  if (result) {

    auto lastOp = future->operators().last();

    // We only add the transformed child data source if the last operator
    // doesn't already have an explicit child data source i.e.
    // hasChildDataSource
    // is true.
    if (!lastOp->hasChildDataSource()) {
      DataSource* newChildDataSource = nullptr;
      if (lastOp->childDataSource() == nullptr) {
        newChildDataSource = new DataSource("Output");
        newChildDataSource->setParent(this);
        addDataSource(newChildDataSource);
        lastOp->setChildDataSource(newChildDataSource);
      }

      lastOp->childDataSource()->setData(future->result());
      lastOp->childDataSource()->dataModified();

      if (newChildDataSource != nullptr) {
        emit lastOp->newChildDataSource(newChildDataSource);
      }
    }

    // Do we have another branch to execute
    if (lastOp->childDataSource() != nullptr) {
      execute(lastOp->childDataSource());
    }
    // The pipeline execution is finished
    else {
      emit finished();
    }

    future->deleteLater();
    if (m_future == future) {
      m_future = nullptr;
    }
  } else {
    future->result()->Delete();
  }
}

void Pipeline::pipelineBranchCanceled()
{
  PipelineWorker::Future* future =
    qobject_cast<PipelineWorker::Future*>(sender());
  future->result()->Delete();
  future->deleteLater();
  if (m_future == future) {
    m_future = nullptr;
  }
}

void Pipeline::pause()
{
  m_paused = true;
}

void Pipeline::resume(bool run)
{
  m_paused = false;
  if (run) {
    execute();
  }
}

void Pipeline::cancel(std::function<void()> canceled)
{
  if (m_future) {
    if (canceled) {
      connect(m_future, &PipelineWorker::Future::canceled, canceled);
    }
    m_future->cancel();
  }
}

bool Pipeline::isRunning()
{
  return m_future != nullptr && m_future->isRunning();
}

DataSource* Pipeline::findTransformedDataSource(DataSource* dataSource)
{
  auto op = findTransformedDataSourceOperator(dataSource);
  if (op != nullptr) {
    return op->childDataSource();
  }

  return nullptr;
}

Operator* Pipeline::findTransformedDataSourceOperator(DataSource* dataSource)
{
  auto operators = dataSource->operators();
  for (auto itr = operators.rbegin(); itr != operators.rend(); ++itr) {
    auto op = *itr;
    // hasChildDataSource is only set by operators that explicitly produce child
    // data sources
    // such as a reconstruction operator. As part of the pipeline execution we
    // do not
    // set that flag, so we currently use it to tell the difference between
    // "explicit"
    // child data sources and those used to represent the transform data source.
    if (!op->hasChildDataSource() && op->childDataSource() != nullptr) {
      return op;
    }
  }

  return nullptr;
}

void Pipeline::addDataSource(DataSource* dataSource)
{
  connect(dataSource, &DataSource::operatorAdded,
          [this](Operator* op) { this->execute(op->dataSource(), true); });
  // Wire up transformModified to execute pipeline
  connect(dataSource, &DataSource::operatorAdded, [this](Operator* op) {
    // Extract out source and execute all.
    connect(op, &Operator::transformModified, this,
            [this]() { this->execute(); });

    // We need to ensure we move add datasource to the end of the branch
    auto operators = op->dataSource()->operators();
    if (operators.size() > 1) {
      auto transformedDataSourceOp =
        this->findTransformedDataSourceOperator(op->dataSource());
      if (transformedDataSourceOp != nullptr) {
        auto transformedDataSource = transformedDataSourceOp->childDataSource();
        transformedDataSourceOp->setChildDataSource(nullptr);
        op->setChildDataSource(transformedDataSource);
        // Delay emitting signal until next event loop
        QTimer::singleShot(
          0, [=] { emit op->dataSourceMoved(transformedDataSource); });
      }
    }
  });
  // Wire up operatorRemoved. TODO We need to check the branch of the
  // pipeline we are currently executing.
  connect(dataSource, &DataSource::operatorRemoved, [this](Operator* op) {
    // Do we need to move the transformed data source, !hasChildDataSource as we
    // don't want to move "explicit" child data sources.
    if (!op->hasChildDataSource() && op->childDataSource() != nullptr) {
      auto transformedDataSource = op->childDataSource();
      auto operators = op->dataSource()->operators();
      // We have an operator to move it to.
      if (!operators.isEmpty()) {
        auto newOp = operators.last();
        op->setChildDataSource(nullptr);
        newOp->setChildDataSource(transformedDataSource);
        emit newOp->dataSourceMoved(transformedDataSource);
      }
      // Clean it up
      else {
        transformedDataSource->removeAllOperators();
        transformedDataSource->deleteLater();
      }
    }

    // If pipeline is running see if we can safely remove the operator
    if (m_future && m_future->isRunning()) {
      // If we can't safely cancel the execution then trigger the rerun of the
      // pipeline.
      if (!m_future->cancel(op)) {
        this->execute(op->dataSource());
      }
    } else {
      // Trigger the pipeline to run
      this->execute(op->dataSource());
    }
  });
}

Pipeline::ImageFuture::ImageFuture(Operator* op,
                                   vtkSmartPointer<vtkImageData> imageData,
                                   PipelineWorker::Future* future,
                                   QObject* parent)
  : QObject(parent), m_operator(op), m_imageData(imageData), m_future(future)
{

  if (m_future != nullptr) {
    connect(m_future, &PipelineWorker::Future::finished, this,
            &Pipeline::ImageFuture::finished);
    connect(m_future, &PipelineWorker::Future::canceled, this,
            &Pipeline::ImageFuture::canceled);
  }
}

Pipeline::ImageFuture::~ImageFuture()
{
  if (m_future != nullptr) {
    m_future->deleteLater();
  }
}

Pipeline::ImageFuture* Pipeline::getCopyOfImagePriorTo(Operator* op)
{
  ImageFuture* imageFuture;

  auto dataSource = m_data;
  auto dataObject = dataSource->copyData();
  vtkSmartPointer<vtkImageData> result(vtkImageData::SafeDownCast(dataObject));
  auto operators = dataSource->operators();
  if (operators.size() > 1) {
    auto index = operators.indexOf(op);
    // Only run operators if we have some to run
    if (index > 0) {
      auto future = m_worker->run(result, operators.mid(0, index));

      imageFuture = new ImageFuture(op, result, future);

      return imageFuture;
    }
  }

  imageFuture = new ImageFuture(op, result);
  // Delay emitting signal until next event loop
  QTimer::singleShot(0, [=] { emit imageFuture->finished(true); });

  return imageFuture;
}

} // tomviz namespace
