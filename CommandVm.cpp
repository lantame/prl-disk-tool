///////////////////////////////////////////////////////////////////////////////
///
/// @file CommandVm.cpp
///
/// Command execution for VM disk images.
///
/// @author mperevedentsev
///
/// Copyright (c) 2005-2015 Parallels IP Holdings GmbH
///
/// This file is part of Virtuozzo Core. Virtuozzo Core is free
/// software; you can redistribute it and/or modify it under the terms
/// of the GNU General Public License as published by the Free Software
/// Foundation; either version 2 of the License, or (at your option) any
/// later version.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with this program; if not, write to the Free Software
/// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
/// 02110-1301, USA.
///
/// Our contact details: Parallels IP Holdings GmbH, Vordergasse 59, 8200
/// Schaffhausen, Switzerland.
///
///////////////////////////////////////////////////////////////////////////////
#include <sys/statvfs.h>
#include <sstream>
#include <iomanip>
#include <fcntl.h>

#include <QFileInfo>
#include <QMap>
#include <boost/scope_exit.hpp>

#include "Command.h"
#include "Util.h"
#include "StringTable.h"
#include "GuestFSWrapper.h"
#include "DiskLock.h"
#include "Errors.h"

using namespace Command;
using namespace GuestFS;

namespace
{

// String constants

const char VIRT_RESIZE[] = "/usr/bin/virt-resize";
const char VIRT_SPARSIFY[] = "/usr/bin/virt-sparsify";
const char GUESTFISH[] = "/usr/bin/guestfish";

const char TMP_IMAGE_EXT[] = ".tmp";

// Numeric constants
enum {SECTOR_SIZE = 512};
enum {GPT_DEFAULT_END_SECTS = 127}; // guestfs somehow uses this value.

// Functions

// TODO: implement progress counters.
/** Callback - output operation progress */
/*
bool OutputCallback(int iComplete, int iTotal, void *pParam)
{
	static int lastCompletion = 0;
	bool bContinue = true;
	(void)pParam;

	if (lastCompletion != iComplete)
	{
		//
		// Console output
		//

		lastCompletion = iComplete;

		// Skip PRL_DISK_PROGRESS_COMPLETED. Should send it by hand, at the end of operation.
		if (iComplete == PRL_DISK_PROGRESS_COMPLETED)
			iComplete = PRL_DISK_PROGRESS_MAX;

		int percents = (100 * iComplete) / iTotal;

		printf("\r%s %u %%", "Operation progress", percents);
		fflush(stdout);
		if (percents >= 100)
			printf("\n");

		//
		// Shared memory output
		//

		if (s_SharedMem != NULL)
		{
			if (s_Version == IMAGE_TOOL_PROTOCOL_VERSION_CURRENT)
			{
				struct ImageToolSharedInfo *sharedInfo = (struct ImageToolSharedInfo *) s_SharedMem->Data();
				if (iComplete > 0)
					// Set the progress count
					sharedInfo->m_Completion = iComplete;

				// Check do we need to cancel?
				if (sharedInfo->m_Break)
				{
					// Cancel requested
					printf("\n%s\n", "Canceling the operation...");
					bContinue = false;
					// NOTE: must not detach shmem here (segfault)
				}
			}
			else  // handle older protocol versions here
				Q_ASSERT_X(false, "shmem communication", "bad shared memory protocol version");
		}
	}

	if (NeedCleanup())
	{
		// Cancel requested
		printf("\n%s\n", "Canceling the operation...");
		bContinue = false;
		// NOTE: must not detach shmem here (segfault)
	}

	return bContinue;
}
*/

Expected<Image::Chain> parseImageChain(const QString &path)
{
	QStringList args;
	args << "info" << "--backing-chain" << "--output=json" << path;
	QByteArray out;
	if (run_prg(QEMU_IMG, args, &out, NULL))
		return Expected<Image::Chain>::fromMessage("Snapshot chain is unavailable");

	QString dirPath = QFileInfo(path).absolutePath();
	Expected<Image::Chain> chain = Image::Parser(dirPath).parse(out);
	if (chain.isOk())
		Logger::info(chain.get().toString() + "\n");
	return chain;
}

quint64 getAvailableSpace(const QString &path)
{
	struct statvfs stat;
	statvfs(QSTR2UTF8(path), &stat);
	return stat.f_bavail * stat.f_bsize;
}

QString getTmpImagePath(const QString &path)
{
	return path + TMP_IMAGE_EXT;
}

quint64 convertMbToBytes(quint64 mb)
{
	return mb * 1024 * 1024;
}

/** Print size using specified units */
QString printSize(quint64 bytes, SizeUnitType unitType)
{
	std::ostringstream out;
	out << std::setw(15);

	switch(unitType)
	{
		case SIZEUNIT_b:
			out << bytes;
			break;
		case SIZEUNIT_K:
			out << qRound64(ceil(bytes/1024.)) << "K";
			break;
		case SIZEUNIT_M:
			out << qRound64(ceil(bytes/1048576.)) << "M";
			break;
		case SIZEUNIT_G:
			out << qRound64(ceil(bytes/1073741824.)) << "G";
			break;
		case SIZEUNIT_T:
			out << qRound64(ceil(bytes/1099511627776.)) << "T";
			break;
		case SIZEUNIT_s:
			out << qRound64(ceil(bytes/512.)) << " sectors";
			break;
		default:
			Q_ASSERT(0);
	}

	return QString(out.str().c_str());
}

////////////////////////////////////////////////////////////
// VirtResize

struct VirtResize
{
	VirtResize(const CallAdapter &adapter):
		m_adapter(adapter)
	{
	}

	VirtResize& expand(const QString &partition)
	{
		m_args << "--expand" << partition;
		return *this;
	}

	VirtResize& shrink(const QString &partition)
	{
		m_args << "--shrink" << partition;
		return *this;
	}

	Expected<void> operator() (const QString &src, const QString &dst)
	{
		m_args << "--machine-readable" << "--ntfsresize-force" << src << dst;
		int ret = m_adapter.run(VIRT_RESIZE, m_args, NULL, NULL);
		Expected<void> res;
		if (ret)
		{
			res = Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
											  .arg(VIRT_RESIZE).arg(m_args.join(" ")).arg(ret));
		}
		m_args.clear();
		return res;
	}

private:
	QStringList m_args;
	CallAdapter m_adapter;
};

////////////////////////////////////////////////////////////
// ResizeData

struct ResizeData
{
	quint64 m_currentSize;
	quint64 m_minSize;
	quint64 m_minSizeKeepFS;
	QString m_lastPartition;
	bool m_fsSupported;
	bool m_partitionSupported;

	ResizeData(quint64 currentSize):
		m_currentSize(currentSize), m_minSize(currentSize),
		m_minSizeKeepFS(currentSize), m_fsSupported(true),
		m_partitionSupported(true)
	{
	}

	void print(const SizeUnitType &unitType) const
	{
		QString warnings;
		if (!m_partitionSupported)
			warnings.append("Unsupported partition\n");
		else if (m_lastPartition.isEmpty())
			warnings.append("No partitions found\n");
		else if (!m_fsSupported)
			warnings.append(IDS_DISK_INFO__RESIZE_WARN_FS_NOTSUPP).append("\n");

		/// Output to console
		Logger::print(IDS_DISK_INFO__HEAD);
		Logger::print(QString("%1%2").arg(IDS_DISK_INFO__SIZE).arg(printSize(m_currentSize, unitType)));
		Logger::print(QString("%1%2").arg(IDS_DISK_INFO__MIN).arg(printSize(m_minSize, unitType)));
		Logger::print(QString("%1%2").arg(IDS_DISK_INFO__MIN_KEEP_FS).arg(printSize(m_minSizeKeepFS, unitType)));

		if (!warnings.isEmpty())
			Logger::error(warnings);
	}
};

////////////////////////////////////////////////////////////
// ResizeHelper

struct ResizeHelper
{
	ResizeHelper(const Image::Info &image,
			const boost::optional<Call> &call = boost::optional<Call>(),
			const boost::optional<Action> &gfsAction = boost::optional<Action>()):
		m_image(image), m_adapter(call), m_call(call), m_gfsAction(gfsAction)
	{
	}

	Expected<Partition::Unit> getLastPartition()
	{
		Expected<Wrapper> gfs = getGFS();
		if (!gfs.isOk())
			return gfs;

		return gfs.get().getLastPartition();
	}

	Expected<ResizeData> getResizeData()
	{
		ResizeData info(m_image.getVirtualSize());
		Expected<Partition::Unit> lastPartition = getLastPartition();
		if (!lastPartition.isOk())
		{
			if (lastPartition.getCode() == ERR_NO_PARTITIONS)
			{
				info.m_minSizeKeepFS = 0;
				return info;
			}
			// Something bad happened.
			return lastPartition;
		}
		info.m_lastPartition = lastPartition.get().getName();

		Expected<Wrapper> gfs = getGFS();
		if (!gfs.isOk())
			return gfs;

		Expected<Partition::Stats> stats = lastPartition.get().getStats();
		if (!stats.isOk())
			return stats;

		quint64 usedSpace = stats.get().end + 1;
		quint64 tail = info.m_currentSize - usedSpace;
		Expected<quint64> overhead = gfs.get().getVirtResizeOverhead();
		if (!overhead.isOk())
			return overhead;
		// We always shrink using virt-resize, so overhead is present.
		info.m_minSizeKeepFS = usedSpace + overhead.get();

		Expected<quint64> partMinSize = lastPartition.get().getMinSize();
		if (!partMinSize.isOk())
		{
			if (partMinSize.getCode() != ERR_UNSUPPORTED_FS)
				return partMinSize;

			info.m_fsSupported = false;
			info.m_minSize = info.m_currentSize - tail + overhead.get();
		}
		else
		{
			Logger::info(QString("Minimum size: %1").arg(partMinSize.get()));
			// total_space - space_after_start_of_last_partition + min_space_needed_for_partition_and_resize
			info.m_minSize = info.m_currentSize - (stats.get().size + tail) +
							 partMinSize.get() + overhead.get();
		}
		return info;
	}

	/* Create image. For debugging needs, it works independently of Call value.
	 * You should remove image using QFile::remove.*/
	Expected<QString> createTmpImage(quint64 mb, const QString &backingFile = QString()) const
	{
		QStringList args;
		args << "create" << "-f" << DISK_FORMAT;
		if (backingFile.isEmpty())
			args << "-o" << "lazy_refcounts=on";
		else
			args << "-o" << QString("backing_file=%1,lazy_refcounts=on").arg(backingFile);

		QString tmpPath = getTmpImagePath(m_image.getFilename());
		args << tmpPath << QString("%1M").arg(mb);
		// Always create image.
		int ret = CallAdapter(Call()).run(QEMU_IMG, args, NULL, NULL);
		if (ret)
		{
			return Expected<QString>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
			                                      .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
		}
		return tmpPath;
	}

	Expected<void> shrinkFSIfNeeded(quint64 mb)
	{
		qint64 delta = (qint64)convertMbToBytes(mb) - (qint64)m_image.getVirtualSize();
		// GuestFS handle wrapper.
		Expected<Wrapper> gfs = getGFS(true);
		if (!gfs.isOk())
			return gfs;
		Expected<Partition::Unit> lastPartition = gfs.get().getLastPartition();
		if (!lastPartition.isOk())
			return lastPartition;
		Expected<Partition::Stats> partStats = lastPartition.get().getStats();
		if (!partStats.isOk())
			return partStats;
		// Free space after last partition.
		quint64 tail = m_image.getVirtualSize() - partStats.get().end - 1;
		Expected<quint64> overhead = gfs.get().getVirtResizeOverhead();
		if (!overhead.isOk())
			return overhead;
		qint64 fsDelta = delta - overhead.get() + tail;
		Logger::info(QString("delta: %1 overhead: %2 tail: %3 fs delta: %4")
					 .arg(delta).arg(overhead.get()).arg(tail).arg(fsDelta));

		// NB: if delta = 1M and overhead = 3M we still need to shrink FS.
		if (fsDelta < 0)
		{
			// Shrinking empty space is not enough.
			// We have to resize filesystem ourselves.
			return lastPartition.get().shrinkFilesystem(-fsDelta);
		}
		return Expected<void>();
	}

	/* Expands partition table(if needed), last partition and its filesystem. */
	Expected<void> expandToFit(quint64 mb, const Wrapper &gfs)
	{
		Expected<void> res;

		/* Getting partition table type fails on non-resized GPT.
		 * So we take it from original image.
		 */
		Expected<Wrapper> oldGFS = getGFS();
		if (!oldGFS.isOk())
			return oldGFS;

		Expected<QString> partTable = oldGFS.get().getPartitionTable();
		if (!partTable.isOk())
			return partTable;

		// Move backup GPT header.
		// We have to do it first because getting partition type
		// with non-moved gpt backup header fails.
		if (partTable.get() == "gpt")
		{
			if (!(res = gfs.expandGPT()).isOk())
				return res;
		}

		Expected<Partition::Unit> lastPartition = gfs.getLastPartition();
		if (!lastPartition.isOk())
			return lastPartition;

		Expected<bool> logical = lastPartition.get().isLogical();
		if (!logical.isOk())
			return logical;

		Expected<Partition::Stats> stats = Partition::Stats();
		if (logical.get())
		{
			// MBR partition table.
			Expected<Partition::Unit> container = gfs.getContainer();
			if (!container.isOk())
				return container;

			// We have to resize extended(container) partition.
			if (!(stats = expandPartition(container.get(), mb,
			                              partTable.get(), gfs)).isOk())
				return stats;
		}

		if (!(stats = expandPartition(lastPartition.get(), mb,
		                              partTable.get(), gfs)).isOk())
			return stats;

		if (!(res = lastPartition.get().resizeFilesystem(stats.get().size)).isOk())
			return res;

		return Expected<void>();
	}

	/* Merge given image into its base, rename base to path. */
	Expected<void> mergeIntoPrevious(const QString &path)
	{
		// External merge
		Expected<Merge::External::mode_type> mode = MergeSnapshots::getExternalMode(m_call);
		if (!mode.isOk())
			return mode;

		Merge::External::Executor external(DiskAware(path), mode.get(), m_call);
		Expected<Image::Chain> chain = parseImageChain(path);
		if (!chain.isOk())
			return chain;

		const QList<Image::Info> &list = chain.get().getList();
		// Take original image and overlay only.
		Image::Chain snapshotChain(list.mid(list.length() - 2));
		// TODO: Any ways to recover?
		return external.execute(snapshotChain);
	}

	Expected<Wrapper> getGFS(bool needWrite = false, const QString &path = QString())
	{
		QString key = path.isEmpty() ? m_image.getFilename() : path;
		QMap<QString, Wrapper>::iterator it = m_gfsMap.find(key);

		if (needWrite && (it == m_gfsMap.end() || it.value().isReadOnly()))
		{
			// call destructor to avoid concurrency
			if (it != m_gfsMap.end())
				m_gfsMap.erase(it);
			// create rw
			Expected<Wrapper> gfs = Wrapper::create(key, m_gfsAction);
			if (!gfs.isOk())
				return gfs;
			m_gfsMap.insert(key, gfs.get());
		}
		else if (it == m_gfsMap.end())
		{
			// create ro
			Expected<Wrapper> gfs = Wrapper::createReadOnly(key, m_gfsAction);
			if (!gfs.isOk())
				return gfs;
			m_gfsMap.insert(key, gfs.get());
		}

		it = m_gfsMap.find(key);
		return it.value();
	}

private:
	Expected<Partition::Stats> expandPartition(
	        const Partition::Unit &partition, quint64 mb,
	        const QString &partTable, const Wrapper &gfs)
	{
		Expected<Partition::Stats> stats = partition.getStats();
		if (!stats.isOk())
			return stats;

		Expected<quint64> sectorSize = gfs.getSectorSize();
		if (!sectorSize.isOk())
			return sectorSize;

		stats = calculateNewPartition(
				mb, stats.get(), sectorSize.get(), partTable);
		if (!stats.isOk())
			return stats;

		Expected<void> res = gfs.resizePartition(
				partition, stats.get().start / sectorSize.get(),
				stats.get().end / sectorSize.get());
		if (!res.isOk())
			return res;
		return stats;
	}

	Expected<Partition::Stats> calculateNewPartition(
			quint64 mb, const Partition::Stats &stats,
			quint64 sectorSize, const QString &partTable)
	{
		Partition::Stats newStats;

		// New partition will end on this sector (including).
		quint64 endSector = convertMbToBytes(mb) / sectorSize - 1;
		if (partTable == "gpt")
		{
			quint64 tail = m_image.getVirtualSize() - stats.end - 1;
			// If gpt backup space was less than our default then we use it.
			endSector = (convertMbToBytes(mb) - qMin(
						tail, GPT_DEFAULT_END_SECTS * sectorSize
						)) / sectorSize - 1;
		}

		newStats.start = stats.start;
		newStats.end = (endSector + 1) * sectorSize - 1;
		newStats.size = newStats.end - newStats.start + 1;
		return newStats;
	}

private:
	const Image::Info &m_image;
	CallAdapter m_adapter;
	// Lazy-initialized.
	QMap<QString, Wrapper> m_gfsMap;
	boost::optional<Call> m_call;
	boost::optional<Action> m_gfsAction;
};

} // namespace

namespace Command
{
namespace Resizer
{

Expected<mode_type> getModeIgnore(const Image::Info &info, quint64 sizeMb)
{
	if (info.getVirtualSize() > convertMbToBytes(sizeMb))
		return mode_type(Ignore::Shrink());
	ResizeHelper helper(info);

	Expected<Wrapper> gfs = helper.getGFS();
	if (!gfs.isOk())
		return Expected<void>(gfs);

	Expected<QString> partTable = gfs.get().getPartitionTable();
	if (!partTable.isOk())
		return Expected<void>(partTable);

	if (partTable.get() == "gpt")
		return mode_type(Gpt<Ignore::Expand>(Ignore::Expand()));
	return mode_type(Ignore::Expand());
}

Expected<mode_type> getModeConsider(const Image::Info &info, quint64 sizeMb)
{
	ResizeHelper helper(info);
	Expected<Partition::Unit> lastPartition = helper.getLastPartition();
	if (lastPartition.isOk())
	{
		Expected<bool> fsSupported = lastPartition.get().isFilesystemSupported();
		if (!fsSupported.isOk())
			return Expected<void>(fsSupported);
		else if (!fsSupported.get())
		{
			// Unsupported fs. Fallback to partition-unaware resize.
			return getModeIgnore(info, sizeMb);
		}

		// Partition-aware resize is safe.
		if (info.getVirtualSize() > convertMbToBytes(sizeMb))
			return mode_type(Consider::Shrink());
		else
			return mode_type(Consider::Expand());
	}
	else if (lastPartition.getCode() == ERR_NO_PARTITIONS)
	{
		// Safe to resize ignoring partitions.
		return getModeIgnore(info, sizeMb);
	}
	else
	{
		// We may destroy data. Refuse.
		return Expected<void>(lastPartition);
	}
}

////////////////////////////////////////////////////////////
// Ignore::Shrink

Expected<void> Ignore::Shrink::execute(
		const Image::Info &image, quint64 sizeMb,
		const boost::optional<Call> &call,
		const boost::optional<GuestFS::Action> &gfsAction) const
{
	ResizeHelper helper(image, call, gfsAction);
	CallAdapter adapter(call);

	Expected<QString> tmpPath = helper.createTmpImage(sizeMb);
	if (!tmpPath.isOk())
		return tmpPath;
	BOOST_SCOPE_EXIT(&tmpPath)
	{
		QFile::remove(tmpPath.get());
	} BOOST_SCOPE_EXIT_END

	Expected<void> res;
	if (!(res = VirtResize(adapter)(image.getFilename(), tmpPath.get())).isOk())
		return res;
	adapter.rename(tmpPath.get(), image.getFilename());
	return res;
}

Expected<void> Ignore::Shrink::checkSpace(const Image::Info &image) const
{
	quint64 avail = getAvailableSpace(image.getFilename());
	quint64 resultSize = image.getActualSize();
	// We copy an image without modifyng anything,
	// so we need equal amount of space.
	if (resultSize > avail)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_NO_FREE_SPACE)
		                                   .arg(resultSize).arg(avail));
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Ignore::Expand

Expected<void> Ignore::Expand::execute(
		const Image::Info &image, quint64 sizeMb,
		const boost::optional<Call> &call) const
{
	CallAdapter adapter(call);
	QStringList args;
	// This is performed in-place.
	args << "resize" << image.getFilename() << QString("%1M").arg(sizeMb);
	int ret = adapter.run(QEMU_IMG, args, NULL, NULL);
	if (ret)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
		                                   .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
	}
	return Expected<void>();
}

Expected<void> Ignore::Expand::checkSpace(
		const Image::Info &image, quint64 sizeMb) const
{
	quint64 avail = getAvailableSpace(image.getFilename());
	qint64 delta = (qint64)convertMbToBytes(sizeMb) - (qint64)image.getVirtualSize();
	Q_ASSERT(delta > 0);
	// We resize in-place, so we should consider only additional space.
	if (delta > 0 && (quint64)delta > avail)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_NO_FREE_SPACE)
		                                   .arg(delta).arg(avail));
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Consider::Shrink

Expected<void> Consider::Shrink::execute(
		const Image::Info &image, quint64 sizeMb,
		const boost::optional<Call> &call,
		const boost::optional<GuestFS::Action> &gfsAction) const
{
	ResizeHelper helper(image, call, gfsAction);
	CallAdapter adapter(call);

	Expected<QString> tmpPath = helper.createTmpImage(sizeMb);
	if (!tmpPath.isOk())
		return tmpPath;
	BOOST_SCOPE_EXIT(&tmpPath)
	{
		QFile::remove(tmpPath.get());
	} BOOST_SCOPE_EXIT_END

	Expected<void> res;
	if (!(res = helper.shrinkFSIfNeeded(sizeMb)).isOk())
		return res;
	Expected<Partition::Unit> lastPartition = helper.getLastPartition();
	if (!lastPartition.isOk())
		return lastPartition;
	if (!(res = VirtResize(adapter).shrink(lastPartition.get().getName())
				(image.getFilename(), tmpPath.get())).isOk())
		return res;
	adapter.rename(tmpPath.get(), image.getFilename());
	return res;
}

Expected<void> Consider::Shrink::checkSpace(const Image::Info &image) const
{
	quint64 avail = getAvailableSpace(image.getFilename());
	// Heuristic estimates: we create a copy of image.
	quint64 resultSize = image.getActualSize();
	if (resultSize > avail)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_NO_FREE_SPACE)
		                                   .arg(resultSize).arg(avail));
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Consider::Expand

Expected<void> Consider::Expand::execute(
		const Image::Info &image, quint64 sizeMb,
		const boost::optional<Call> &call,
		const boost::optional<GuestFS::Action> &gfsAction) const
{
	// TODO: Implement in-place.
	ResizeHelper helper(image, call, gfsAction);
	CallAdapter adapter(call);

	// Create overlay to operate on.
	Expected<QString> tmpPath = helper.createTmpImage(sizeMb, image.getFilename());
	if (!tmpPath.isOk())
		return tmpPath;
	BOOST_SCOPE_EXIT(&tmpPath)
	{
		QFile::remove(tmpPath.get());
	} BOOST_SCOPE_EXIT_END

	Expected<void> res;
	Expected<Wrapper> gfs = helper.getGFS(true, tmpPath.get());
	if (!gfs.isOk())
		return gfs;

	// Resize partition table, partition and fs.
	if (!(res = helper.expandToFit(sizeMb, gfs.get())).isOk())
		return res;

	// External-merge overlay into original image.
	if (!(res = helper.mergeIntoPrevious(tmpPath.get())).isOk())
		return res;

	// External merge removes backing images and leaves only top-most one.
	// We need original image, so we rename the result.
	adapter.rename(tmpPath.get(), image.getFilename());
	return res;
}

Expected<void> Consider::Expand::checkSpace(
		const Image::Info &image, quint64 sizeMb) const
{
	quint64 avail = getAvailableSpace(image.getFilename());
	// Heuristic estimates: we only spend space for filesystem expanding.
	const double FS_OVERHEAD = 0.05;
	quint64 resultSize = (convertMbToBytes(sizeMb)) * FS_OVERHEAD;
	if (resultSize > avail)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_NO_FREE_SPACE)
		                                   .arg(resultSize).arg(avail));
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Gpt

template <>
Expected<void> Gpt<Ignore::Expand>::execute(
		const Image::Info &image, quint64 sizeMb,
		const boost::optional<Call> &call,
		const boost::optional<GuestFS::Action> &gfsAction) const
{
	Expected<void> res;
	if (!(res = m_mode.execute(image, sizeMb, call)).isOk())
		return res;

	// Windows does not see additional space if backup GPT header is not moved.
	// So we have to move it to the end of the disk.
	ResizeHelper helper(image, call, gfsAction);
	Expected<Wrapper> gfs = helper.getGFS(true);
	if (!gfs.isOk())
		return gfs;

	return gfs.get().expandGPT();
}

} // namespace Resizer

namespace Visitor
{

////////////////////////////////////////////////////////////
// Execute

struct Execute: boost::static_visitor<Expected<void> >
{
	template <class T>
	Expected<void> operator() (T &executor) const
	{
		return executor.execute();
	}
};

////////////////////////////////////////////////////////////
// Space

struct Space: boost::static_visitor<quint64>
{
	Space(const Image::Info &info):
		m_info(info)
	{
	}

	template <class T>
	quint64 operator() (const T &variant) const
	{
		return variant.getNeededSpace(m_info);
	}

private:
	const Image::Info &m_info;
};

////////////////////////////////////////////////////////////
// Resize

struct Resize: boost::static_visitor<Expected<void> >
{
	Resize(const Image::Info &image, quint64 sizeMb,
		   const boost::optional<Call> &call,
		   const boost::optional<Action> &gfsAction):
		m_image(image), m_sizeMb(sizeMb),
		m_call(call), m_gfsAction(gfsAction)
	{
	}

	template <class T>
	Expected<void> operator() (const T &mode) const;

	template <class T>
	Expected<void> checkSpace(const T &mode) const;

private:
	Image::Info m_image;
	quint64 m_sizeMb;
	boost::optional<Call> m_call;
	boost::optional<Action> m_gfsAction;
};

template <>
Expected<void> Resize::operator() (const Resizer::Ignore::Expand &mode) const
{
	Expected<void> res = mode.checkSpace(m_image, m_sizeMb);
	if (!res.isOk())
		return res;
	return mode.execute(m_image, m_sizeMb, m_call);
}

template <class T>
Expected<void> Resize::operator() (const T &mode) const
{
	Expected<void> res = checkSpace(mode);
	if (!res.isOk())
		return res;
	return mode.execute(m_image, m_sizeMb, m_call, m_gfsAction);
}

template<> Expected<void> Resize::checkSpace(
		const Resizer::Gpt<Resizer::Ignore::Expand> &mode) const
{
	return mode.getMode().checkSpace(m_image, m_sizeMb);
}

template<> Expected<void> Resize::checkSpace(
		const Resizer::Ignore::Expand &mode) const
{
	return mode.checkSpace(m_image, m_sizeMb);
}

template<> Expected<void> Resize::checkSpace(
		const Resizer::Consider::Expand &mode) const
{
	return mode.checkSpace(m_image, m_sizeMb);
}

template<class T>
Expected<void> Resize::checkSpace(const T &mode) const
{
	return mode.checkSpace(m_image);
}

////////////////////////////////////////////////////////////
// PreConvert

struct PreConvert
{
	explicit PreConvert(const CallAdapter &adapter):
		m_adapter(adapter)
	{
	}

	Expected<QString> operator()(const QString &path, const QString &mode) const
	{
		QString tmpPath = getTmpImagePath(path);
		QStringList args;
		args << "convert" << "-O" << DISK_FORMAT << "-o"
			 << QString("preallocation=%1,lazy_refcounts=on").arg(mode)
			 << path << tmpPath;

		int ret = m_adapter.run(QEMU_IMG, args, NULL, NULL);
		if (ret)
		{
			// Remove temporary image.
			m_adapter.remove(tmpPath);
			return Expected<QString>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
											      .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
		}
		return tmpPath;
	}

private:
	CallAdapter m_adapter;
};

////////////////////////////////////////////////////////////
// Convert

struct Convert: boost::static_visitor<Expected<void> >
{
	Convert(const Image::Info &info, const boost::optional<Call> &call):
		m_info(info), m_adapter(call), m_preConv(m_adapter)
	{
	}

	Expected<void> operator()(const Preallocation::Plain &mode) const
	{
		Expected<QString> tmpPath = m_preConv(mode.getDiskPath(), "off");
		if (!tmpPath.isOk())
			return tmpPath;

		Expected<void> alloc = mode.allocate(tmpPath.get(), m_info.getVirtualSize());
		if (!alloc.isOk())
		{
			m_adapter.remove(tmpPath.get());
			return alloc;
		}

		return mode.rename(tmpPath.get());
	}

	Expected<void> operator()(const Preallocation::Expanding &mode) const
	{
		Expected<QString> tmpPath = m_preConv(mode.getDiskPath(), "metadata");
		if (!tmpPath.isOk())
			return tmpPath;
		return mode.rename(tmpPath.get());
	}

private:
	Image::Info m_info;
	CallAdapter m_adapter;
	PreConvert m_preConv;
};

namespace Merge
{

struct External: boost::static_visitor<Expected<void> >
{
	External(const Image::Chain &snapshotChain, const CallAdapter &adapter):
	   m_snapshotChain(snapshotChain), m_adapter(adapter)
	{
	}

	template <class T>
	Expected<void> operator()(const T &mode) const
	{
		QList<Image::Info> chain = m_snapshotChain.getList();
		quint64 avail = getAvailableSpace(chain.first().getFilename()),
				delta = mode.getNeededSpace(m_snapshotChain);

		if (delta > avail)
		{
			return Expected<void>::fromMessage(QString(IDS_ERR_NO_FREE_SPACE)
											   .arg(delta).arg(avail));
		}

		Expected<void> res = mode.doCommit(chain);
		if (!res.isOk())
			return res;

		m_adapter.rename(chain.first().getFilename(), chain.last().getFilename());

		// Base does not exist anymore. Preserve current image.
		Q_FOREACH(const Image::Info &info, chain.mid(1, chain.length() - 2))
			m_adapter.remove(info.getFilename());

		return Expected<void>();
	}

private:
	const Image::Chain &m_snapshotChain;
	CallAdapter m_adapter;
};

} // namespace Merge

} // namespace Visitor

////////////////////////////////////////////////////////////
// Resize

Expected<void> Resize::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard = DiskLockGuard::openWrite(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;
	Expected<Image::Chain> result = parseImageChain(getDiskPath());
	if (!result.isOk())
		return result;
	Image::Chain snapshotChain = result.get();
	if (convertMbToBytes(m_sizeMb) == snapshotChain.getList().last().getVirtualSize())
		return Expected<void>();

	Expected<Resizer::mode_type> mode = m_resizeLastPartition ?
		Resizer::getModeConsider(snapshotChain.getList().last(), m_sizeMb) :
		Resizer::getModeIgnore(snapshotChain.getList().last(), m_sizeMb);
	if (!mode.isOk())
		return mode;

	return boost::apply_visitor(Visitor::Resize(
				snapshotChain.getList().last(),
				m_sizeMb, m_call, m_gfsAction), mode.get());
}

////////////////////////////////////////////////////////////
// ResizeInfo

Expected<void> ResizeInfo::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard = DiskLockGuard::openRead(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;
	Expected<Image::Chain> result = parseImageChain(getDiskPath());
	if (!result.isOk())
		return result;
	Image::Chain snapshotChain = result.get();
	ResizeHelper resizer(snapshotChain.getList().last());
	Expected<ResizeData> infoRes = resizer.getResizeData();
	if (!infoRes.isOk())
		return infoRes;
	infoRes.get().print(m_unitType);
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Compact

Expected<void> Compact::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard = DiskLockGuard::openWrite(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;
	CallAdapter adapter(m_call);
	QStringList args;
	args << "--machine-readable" << "--in-place" << getDiskPath();
	int ret = adapter.run(VIRT_SPARSIFY, args, NULL, NULL);
	if (ret)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
										   .arg(VIRT_SPARSIFY).arg(args.join(" ")).arg(ret));
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// CompactInfo

Expected<void> CompactInfo::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard = DiskLockGuard::openRead(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;
	Expected<Image::Chain> result = parseImageChain(getDiskPath());
	if (!result.isOk())
		return result;
	Image::Chain snapshotChain = result.get();

	quint64 blockSize, size, allocated, used;
	{
		// GuestFS handle wrapper.
		Expected<Wrapper> gfsRes = Wrapper::createReadOnly(
				snapshotChain.getList().last().getFilename());
		if (!gfsRes.isOk())
			return gfsRes;
		const Wrapper& gfs = gfsRes.get();
		Expected<QList<Partition::Unit> > partitions = gfs.getPartitions();
		if (!partitions.isOk())
			return partitions;
		quint64 free = 0;
		Q_FOREACH(const Partition::Unit& part, partitions.get())
		{
			Expected<struct statvfs> stats = part.getFilesystemStats();
			if (!stats.isOk())
				return stats;
			free += stats.get().f_bfree * stats.get().f_frsize;
		}
		size = snapshotChain.getList().last().getVirtualSize();
		// Approximate: qemu-img does not provide a way to get allocated block count.
		allocated = snapshotChain.getList().last().getActualSize();
		used = size - free;
		Expected<quint64> bsizeRes = gfs.getBlockSize();
		if (!bsizeRes.isOk())
			return bsizeRes;
		blockSize = bsizeRes.get();
	}

	Logger::print(QString("%1%2").arg(IDS_DISK_INFO__BLOCK_SIZE).arg(blockSize / SECTOR_SIZE, 15));
	Logger::print(QString("%1%2").arg(IDS_DISK_INFO__BLOCKS_TOTAL).arg(size / blockSize, 15));
	Logger::print(QString("%1%2").arg(IDS_DISK_INFO__BLOCKS_ALLOCATED).arg(allocated / blockSize, 15));
	Logger::print(QString("%1%2").arg(IDS_DISK_INFO__BLOCKS_USED).arg(used / blockSize, 15));
	return Expected<void>();
}

namespace Merge
{
namespace External
{

////////////////////////////////////////////////////////////
// Direct

quint64 Direct::getNeededSpace(const Image::Chain &snapshotChain) const
{
	quint64	virtualSizeMax = snapshotChain.getVirtualSizeMax();
	const QList<Image::Info> chain = snapshotChain.getList();
	// A'[n] = A[n]
	quint64 delta = 0, prevActualSize = chain.last().getActualSize();
	for (int i = chain.length() - 2; i >= 0; --i)
	{
		// A'[i] = Min(V, Sum(A[i], A'[i+1]))
		quint64 actualSize = qMin(virtualSizeMax, chain[i].getActualSize() + prevActualSize);
		delta += actualSize - chain[i].getActualSize();
		prevActualSize = actualSize;
	}
	return delta;
}

Expected<void> Direct::doCommit(const QList<Image::Info> &chain) const
{
	int ret;
	QStringList args;
	args << "commit" << "-b" << chain.first().getFilename() << chain.last().getFilename();
	ret = m_adapter.run(QEMU_IMG, args, NULL, NULL);
	if (ret)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
										   .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Sequential

quint64 Sequential::getNeededSpace(const Image::Chain &snapshotChain) const
{
	// Base image is resized in-place.
	quint64	actualSizeSum = snapshotChain.getActualSizeSum(),
			virtualSizeMax = snapshotChain.getVirtualSizeMax();

	quint64 resultSize = qMin(actualSizeSum, virtualSizeMax);
	return resultSize - snapshotChain.getList().first().getActualSize();
}

Expected<void> Sequential::doCommit(const QList<Image::Info> &chain) const
{
	int ret;
	QStringList args;
	args << "commit";
	for (int i = chain.length() - 1; i > 0; --i)
	{
		args << chain[i].getFilename();
		ret = m_adapter.run(QEMU_IMG, args, NULL, NULL);
		if (ret)
		{
			return Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
											   .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
		}
		args.removeLast();
	}
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Executor

Expected<void> Executor::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard =
		DiskLockGuard::openWrite(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;

	Expected<Image::Chain> result = parseImageChain(getDiskPath());
	if (!result.isOk())
		return result;

	Image::Chain snapshotChain = result.get();
	return execute(snapshotChain);
}

Expected<void> Executor::execute(const Image::Chain &snapshotChain) const
{
	if (snapshotChain.getList().length() <= 1)
		return Expected<void>();

	return boost::apply_visitor(Visitor::Merge::External(
				snapshotChain, m_adapter), m_mode);
}

} // namespace External

////////////////////////////////////////////////////////////
// Internal

Expected<void> Internal::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard = DiskLockGuard::openWrite(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;
	int ret;

	QStringList args;
	args << "snapshot" << "-l" << getDiskPath();
	QByteArray out;
	if ((ret = run_prg(QEMU_IMG, args, &out, NULL)))
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
										   .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
	}

	//                   | ID  |     TAG     | VMSIZE|   DATE                      TIME        |
	QRegExp snapshotRE("^\\d+\\s+(\\S.*\\S)\\s+\\d+\\s+\\d{4}-\\d{2}-\\d{2}");
	QStringList lines = QString(out).split('\n');
	Q_FOREACH(const QString &line, lines)
	{
		if (snapshotRE.indexIn(line) < 0)
			continue;
		args.clear();
		args << "snapshot" << "-d" << snapshotRE.cap(1) << getDiskPath();
		if ((ret = m_adapter.run(QEMU_IMG, args, NULL, NULL)))
		{
			return Expected<void>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
											   .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
		}
	}

	return Expected<void>();
}

} // namespace Merge

////////////////////////////////////////////////////////////
// MergeSnapshots

Expected<Merge::External::mode_type>
MergeSnapshots::getExternalMode(const boost::optional<Call> &call)
{
	using namespace Merge::External;
	QByteArray out;
	QStringList args;
	args << "--help";
	int ret = run_prg(QEMU_IMG, args, &out, NULL);
	if (ret)
	{
		return Expected<mode_type>::fromMessage(QString(IDS_ERR_SUBPROGRAM_RETURN_CODE)
		                                        .arg(QEMU_IMG).arg(args.join(" ")).arg(ret));
	}

	bool baseSupported = QString(out).contains(QRegExp("^\\s*commit.*-b.*$"));
	Logger::info(QString("Backing file specification [-b] is %1supported")
	             .arg(baseSupported ? "" : "not "));
	if (baseSupported)
		return mode_type(Direct(call));
	return mode_type(Sequential(call));
}

Expected<void> MergeSnapshots::execute() const
{
	return boost::apply_visitor(Visitor::Execute(), m_executor);
}

namespace Preallocation
{

////////////////////////////////////////////////////////////
// Plain

Expected<void> Plain::allocate(const QString &path, quint64 size) const
{
	Logger::info(QString("posix_fallocate(open(%1), 0, %2)").arg(path).arg(size));
	if (!getCall())
		return Expected<void>();

	QFile file(path);
	if (!file.open(QIODevice::ReadWrite))
		return Expected<void>::fromMessage("Cannot open temporary image");
	int ret = posix_fallocate(file.handle(), 0, size);
	file.close();
	if (ret)
		return Expected<void>::fromMessage("Cannot posix_fallocate() image");
	return Expected<void>();
}

////////////////////////////////////////////////////////////
// Expanding

Expected<void> Expanding::rename(const QString &tmpPath) const
{
	CallAdapter(m_call).rename(tmpPath, getDiskPath());
	return Expected<void>();
}

} // namespace Preallocation

////////////////////////////////////////////////////////////
// Convert

Expected<void> Convert::execute() const
{
	Expected<boost::shared_ptr<DiskLockGuard> > hddGuard = DiskLockGuard::openWrite(getDiskPath());
	if (!hddGuard.isOk())
		return hddGuard;
	Expected<Image::Chain> result = parseImageChain(getDiskPath());
	if (!result.isOk())
		return result;
	Image::Chain snapshotChain = result.get();
	// qemu-img does not support preallocation change while preserving backing chain.
	if (snapshotChain.getList().length() > 1)
		return Expected<void>::fromMessage(IDS_ERR_CANNOT_CONVERT_NEED_MERGE);

	quint64 avail = getAvailableSpace(getDiskPath());
	quint64 needed = boost::apply_visitor(
			Visitor::Space(snapshotChain.getList().last()), m_preallocation);
	if (needed > avail)
	{
		return Expected<void>::fromMessage(QString(IDS_ERR_NO_FREE_SPACE)
							   .arg(needed).arg(avail));
	}

	return boost::apply_visitor(Visitor::Convert(
				snapshotChain.getList().last(), m_call), m_preallocation);
}

} // namespace Command
