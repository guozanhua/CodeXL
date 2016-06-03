//=====================================================================

//=====================================================================

#include <AMDTGpuProfiling/gpTraceSummaryWidget.h>
#include <AMDTGpuProfiling/gpStringConstants.h>
#include <AMDTApplicationFramework/Include/afProgressBarWrapper.h>

const int DEFAULT_PROGRESS_FACTOR = 10000;
static const char* tab_captions[] = { GPU_STR_API_Summary, GPU_STR_GPU_Summary, GPU_STR_Command_Lists_Summary, GPU_STR_Command_Buffers_Summary };

/////////////////////////////////////////////////////////////////////////////////////
// gpTraceSummaryWidget

gpTraceSummaryWidget::gpTraceSummaryWidget(QWidget* pParent)
    : acTabWidget(pParent), m_pTraceView(nullptr), m_useTimelineSelectionScope(false), m_timelineAbsoluteStart(0), m_timelineStart(0), m_timelineEnd(0)
{
    for (int nTab = 0; nTab < eCallType::MAX_TYPE; nTab++)
    {
        m_tabs[nTab] = nullptr;
    }
}

gpTraceSummaryWidget::~gpTraceSummaryWidget()
{
}

void gpTraceSummaryWidget::Init(gpTraceDataContainer* pDataContainer, gpTraceView* pSessionView, quint64 timelineStartTime, quint64 timelineRange)
{
    m_pTraceView = pSessionView;

    int numItems = pDataContainer->QueueItemsCount() + pDataContainer->ThreadsCount();

    afProgressBarWrapper::instance().ShowProgressDialog(L"Loading Summary", numItems * DEFAULT_PROGRESS_FACTOR);
    afProgressBarWrapper::instance().incrementProgressBar();

    m_timelineAbsoluteStart = timelineStartTime;
    m_timelineStart = timelineStartTime;
    m_timelineEnd = m_timelineStart + timelineRange;

    for (int i = 0; i < eCallType::MAX_TYPE; i++)
    {
        if (i == eCallType::API_CALL || i == eCallType::GPU_CALL)
        {
            m_tabs[i] = new gpTraceSummaryTab((eCallType)i, m_timelineAbsoluteStart);
        }
        else
        {
            m_tabs[i] = new gpCommandListSummaryTab((eCallType)i, m_timelineAbsoluteStart);
        }

        bool rc = m_tabs[i]->Init(pDataContainer, pSessionView, timelineStartTime, timelineRange);
        GT_ASSERT(rc);
        if (i == eCallType::API_CALL || i == eCallType::GPU_CALL || 
            i == eCallType::COMMAND_LIST && pDataContainer->SessionAPIType() == ProfileSessionDataItem::ProfileItemAPIType::DX12_API_PROFILE_ITEM)
        {
            addTab(m_tabs[i], QIcon(), tab_captions[i]);
        }
        else
        {
            addTab(m_tabs[i], QIcon(), tab_captions[i+1]);
        }

        rc = connect(this, SIGNAL(currentChanged(int)), this, SLOT(OnCurrentChanged(int)));
        GT_ASSERT(rc);
        rc = connect(m_tabs[i], SIGNAL(TabUseTimelineSelectionScopeChanged(bool)), this, SLOT(OnUseTimelineSelectionScopeChanged(bool)));
        GT_ASSERT(rc);
        rc = connect(m_tabs[i], SIGNAL(TabSummaryItemClicked(ProfileSessionDataItem*)), this, SLOT(OnTabSummaryItemClicked(ProfileSessionDataItem*)));
        GT_ASSERT(rc);
        if (i == eCallType::COMMAND_LIST)
        {
            gpCommandListSummaryTab* pCmdListTab = dynamic_cast<gpCommandListSummaryTab*>(m_tabs[i]);
            GT_ASSERT(pCmdListTab != nullptr);
            rc = connect(pCmdListTab, SIGNAL(TabSummaryCmdListDoubleClicked(const QString&)), this, SLOT(OnTabSummaryCmdListDoubleClicked(const QString&)));
            GT_ASSERT(rc);
        }

        afProgressBarWrapper::instance().incrementProgressBar();
    }

    afProgressBarWrapper::instance().hideProgressBar();

    setCurrentIndex(COMMAND_LIST);
}

void gpTraceSummaryWidget::SelectAPIRowByCallName(const QString& callName)
{
    GT_IF_WITH_ASSERT(m_tabs[API_CALL] != nullptr)
    {
        gpTraceSummaryTable* pSummaryTable = dynamic_cast<gpTraceSummaryTable*>(m_tabs[API_CALL]->m_pSummaryTable);
        GT_ASSERT(pSummaryTable != nullptr);
        pSummaryTable->SelectRowByCallName(callName);
    }
}

void gpTraceSummaryWidget::SelectGPURowByCallName(const QString& callName)
{
    GT_IF_WITH_ASSERT(m_tabs[GPU_CALL] != nullptr)
    {
        gpTraceSummaryTable* pSummaryTable = dynamic_cast<gpTraceSummaryTable*>(m_tabs[GPU_CALL]->m_pSummaryTable);
        GT_ASSERT(pSummaryTable != nullptr);
        pSummaryTable->SelectRowByCallName(callName);
    }
}

void gpTraceSummaryWidget::ClearAPISelection()
{
    GT_IF_WITH_ASSERT(m_tabs[API_CALL] != nullptr)
    {
        m_tabs[API_CALL]->m_pSummaryTable->clearSelection();
    }
}

void gpTraceSummaryWidget::ClearGPUSelection()
{
    GT_IF_WITH_ASSERT(m_tabs[GPU_CALL] != nullptr)
    {
        m_tabs[GPU_CALL]->m_pSummaryTable->clearSelection();
    }
}

void gpTraceSummaryWidget::OnTimelineChanged(const QPointF& rangePoint, bool isRelativeRangeStartTime)
{
    int activeTabIndex = currentIndex();

    if (isRelativeRangeStartTime)
    {
        m_timelineStart = m_timelineAbsoluteStart + rangePoint.x() ;
        m_timelineEnd = m_timelineAbsoluteStart + rangePoint.y();
    }
    else
    {
        m_timelineStart = rangePoint.x();
        m_timelineEnd = m_timelineStart + rangePoint.y();
    }

    for (auto tab : m_tabs)
    {
        if (tab != nullptr)
        {
            tab->OnTimelineChanged(m_timelineStart, m_timelineEnd);
        }
    }

    if (m_useTimelineSelectionScope == true)
    {
        GT_IF_WITH_ASSERT(m_tabs[activeTabIndex] != nullptr && m_tabs[activeTabIndex])
        {
            m_tabs[activeTabIndex]->RefreshAndMaintainSelection(true);
        }
    }
}
void gpTraceSummaryWidget::OnTimelineFilterChanged(QMap<QString, bool>& threadNameVisibilityMap)
{
    GT_UNREFERENCED_PARAMETER(threadNameVisibilityMap);
    int activeTabIndex = currentIndex();
    GT_IF_WITH_ASSERT(m_tabs[activeTabIndex] != nullptr && m_tabs[activeTabIndex])
    {
        m_tabs[activeTabIndex]->OnTimelineFilterChanged();
    }
}
void gpTraceSummaryWidget::OnUseTimelineSelectionScopeChanged(bool check)
{
    m_useTimelineSelectionScope = check;
}

void gpTraceSummaryWidget::OnFind()
{
    // Get the current tab and find the requested text in it
    gpSummaryTab* pCurrentTab = qobject_cast<gpSummaryTab*>(currentWidget());

    if (pCurrentTab != nullptr)
    {
        pCurrentTab->OnFind();
    }
}

void gpTraceSummaryWidget::OnEditSelectAll()
{
    // Get the current tab and apply the select all command on it
    gpSummaryTab* pCurrentTab = qobject_cast<gpSummaryTab*>(currentWidget());

    if (pCurrentTab != nullptr)
    {
        pCurrentTab->OnEditSelectAll();
    }
}

void gpTraceSummaryWidget::OnEditCopy()
{
    // Get the current tab and apply the copy command on it
    gpSummaryTab* pCurrentTab = qobject_cast<gpSummaryTab*>(currentWidget());

    if (pCurrentTab != nullptr)
    {
        pCurrentTab->OnEditCopy();
    }
}

void gpTraceSummaryWidget::OnCurrentChanged(int activeTabIndex)
{
    GT_IF_WITH_ASSERT(m_tabs[activeTabIndex] != nullptr && m_tabs[activeTabIndex])
    {
        m_tabs[activeTabIndex]->SetTimelineScope(m_useTimelineSelectionScope, m_timelineStart, m_timelineEnd);
        m_tabs[activeTabIndex]->RefreshAndMaintainSelection(m_useTimelineSelectionScope);
    }
}

void gpTraceSummaryWidget::OnTabSummaryItemClicked(ProfileSessionDataItem* pItem)
{
    emit SummaryItemClicked(pItem);
}

void gpTraceSummaryWidget::SelectCommandList(const QString& commandListName)
{
    gpCommandListSummaryTab* pCurrentTab = qobject_cast<gpCommandListSummaryTab*>(currentWidget());
    if (pCurrentTab != nullptr)
    {
        pCurrentTab->SelectCommandList(commandListName);
    }
}

void gpTraceSummaryWidget::OnTabSummaryCmdListDoubleClicked(const QString& cmdList)
{
    emit SummaryCmdListDoubleClicked(cmdList);
}