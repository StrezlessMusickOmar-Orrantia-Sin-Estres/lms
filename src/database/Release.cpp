/*
 * Copyright (C) 2015 Emeric Poupon
 *
 * This file is part of LMS.
 *
 * LMS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LMS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Release.hpp"

#include "Cluster.hpp"
#include "DbArtist.hpp"
#include "SqlQuery.hpp"
#include "Track.hpp"

namespace Database
{

Release::Release(const std::string& name, const std::string& MBID)
: _name(std::string(name, 0 , _maxNameLength)),
_MBID(MBID)
{

}

std::vector<Release::pointer>
Release::getByName(Wt::Dbo::Session& session, const std::string& name)
{
	Wt::Dbo::collection<Release::pointer> res = session.find<Release>().where("name = ?").bind( std::string(name, 0, _maxNameLength) );
	return std::vector<Release::pointer>(res.begin(), res.end());
}

Release::pointer
Release::getByMBID(Wt::Dbo::Session& session, const std::string& mbid)
{
	return session.find<Release>().where("mbid = ?").bind(mbid);
}

Release::pointer
Release::getById(Wt::Dbo::Session& session, IdType id)
{
	return session.find<Release>().where("id = ?").bind(id);
}

Release::pointer
Release::create(Wt::Dbo::Session& session, const std::string& name, const std::string& MBID)
{
	return session.add(std::make_unique<Release>(name, MBID));
}

std::vector<Release::pointer>
Release::getAll(Wt::Dbo::Session& session, int offset, int size)
{
	Wt::Dbo::collection<pointer> res = session.find<Release>().offset(offset).limit(size);
	return std::vector<pointer>(res.begin(), res.end());
}

std::vector<Release::pointer>
Release::getAllOrphans(Wt::Dbo::Session& session)
{
	Wt::Dbo::collection<Release::pointer> res = session.query<Wt::Dbo::ptr<Release>>("select r from release r LEFT OUTER JOIN Track t ON r.id = t.release_id WHERE t.id IS NULL");

	return std::vector<pointer>(res.begin(), res.end());
}

static
Wt::Dbo::Query<Release::pointer>
getQuery(Wt::Dbo::Session& session,
			const std::set<IdType>& clusterIds,
			const std::vector<std::string> keywords)
{
	WhereClause where;

        std::ostringstream oss;
	oss << "SELECT DISTINCT r FROM release r";

	for (auto keyword : keywords)
		where.And(WhereClause("r.name LIKE ?")).bind("%%" + keyword + "%%");

	if (!clusterIds.empty())
	{
		oss << " INNER JOIN track t ON t.release_id = r.id INNER JOIN cluster c ON c.id = t_c.cluster_id INNER JOIN track_cluster t_c ON t_c.track_id = t.id";

		WhereClause clusterClause;

		for (auto id : clusterIds)
			clusterClause.Or(WhereClause("c.id = ?")).bind(std::to_string(id));

		where.And(clusterClause);
	}

	oss << " " << where.get();

	if (!clusterIds.empty())
		oss << " GROUP BY t.id HAVING COUNT(*) = " << clusterIds.size();

	oss << " ORDER BY r.name";

	Wt::Dbo::Query<Release::pointer> query = session.query<Release::pointer>( oss.str() );

	for (const std::string& bindArg : where.getBindArgs())
		query.bind(bindArg);

	return query;
}

std::vector<Release::pointer>
Release::getByFilter(Wt::Dbo::Session& session,
		const std::set<IdType>& clusterIds,
		const std::vector<std::string> keywords,
		int offset, int size, bool& moreResults)
{
	Wt::Dbo::collection<pointer> collection = getQuery(session, clusterIds, keywords)
		.limit(size != -1 ? size + 1 : -1)
		.offset(offset);

	auto res = std::vector<pointer>(collection.begin(), collection.end());

	if (size != -1 && res.size() == static_cast<std::size_t>(size) + 1)
	{
		moreResults = true;
		res.pop_back();
	}
	else
		moreResults = false;

	return res;
}

boost::optional<int>
Release::getReleaseYear(bool original) const
{
	assert(session());

	std::string field = original ? "t.original_year" : "t.year";

	Wt::Dbo::collection<int> dates = session()->query<int>(
			std::string("SELECT ") + field + " FROM track t INNER JOIN release r ON r.id = t.release_id")
		.where("r.id = ?")
		.groupBy(field)
		.bind(this->id());

	/* various dates, no date */
	if (dates.empty() || dates.size() > 1)
		return boost::none;

	auto date = dates.front();

	if (date > 0)
		return date;
	else
		return boost::none;
}

std::vector<Wt::Dbo::ptr<Artist>>
Release::getArtists() const
{
	assert(self());
	assert(IdIsValid(self()->id()));
	assert(session());

	Wt::Dbo::collection<Wt::Dbo::ptr<Artist>> res = session()->query<Wt::Dbo::ptr<Artist>>(
			"SELECT DISTINCT a FROM artist a INNER JOIN release r ON t.artist_id = a.id INNER JOIN track t ON t.release_id = r.id")
		.where("r.id = ?")
		.bind(id());

	return std::vector<Wt::Dbo::ptr<Artist>>(res.begin(), res.end());
}

bool
Release::hasVariousArtists() const
{
	// TODO optimize
	return getArtists().size() > 1;
}

std::vector<Wt::Dbo::ptr<Track>>
Release::getTracks(const std::set<IdType>& clusterIds) const
{
	assert(self());
	assert(self()->id() != Wt::Dbo::dbo_traits<Release>::invalidId() );
	assert(session());

	WhereClause where;

	std::ostringstream oss;
	oss << "SELECT t FROM track t INNER JOIN release r ON t.release_id = r.id";

	if (!clusterIds.empty())
	{
		oss << " INNER JOIN cluster c ON c.id = t_c.cluster_id INNER JOIN track_cluster t_c ON t_c.track_id = t.id";

		WhereClause clusterClause;

		for (auto id : clusterIds)
			clusterClause.Or(WhereClause("c.id = ?")).bind(std::to_string(id));

		where.And(clusterClause);
	}

	where.And(WhereClause("r.id = ?")).bind(std::to_string(id()));

	oss << " " << where.get();

	if (!clusterIds.empty())
		oss << " GROUP BY t.id HAVING COUNT(*) = " << clusterIds.size();

	oss << " ORDER BY t.disc_number,t.track_number";

	Wt::Dbo::Query<Track::pointer> query = session()->query<Track::pointer>( oss.str() );

	for (const std::string& bindArg : where.getBindArgs())
	{
		query.bind(bindArg);
	}

	Wt::Dbo::collection< Wt::Dbo::ptr<Track> > res = query;

	return std::vector< Wt::Dbo::ptr<Track> > (res.begin(), res.end());
}

std::vector<std::vector<Wt::Dbo::ptr<Cluster>>>
Release::getClusterGroups(std::vector<ClusterType::pointer> clusterTypes, std::size_t size) const
{
	assert(self());
	assert(self()->id() != Wt::Dbo::dbo_traits<Artist>::invalidId() );
	assert(session());

	WhereClause where;

	std::ostringstream oss;

	oss << "SELECT c from cluster c INNER JOIN track t ON c.id = t_c.cluster_id INNER JOIN track_cluster t_c ON t_c.track_id = t.id INNER JOIN cluster_type c_type ON c.cluster_type_id = c_type.id INNER JOIN release r ON t.release_id = r.id ";

	where.And(WhereClause("r.id = ?")).bind(std::to_string(self()->id()));
	{
		WhereClause clusterClause;
		for (auto clusterType : clusterTypes)
			clusterClause.Or(WhereClause("c_type.id = ?")).bind(std::to_string(clusterType.id()));
		where.And(clusterClause);
	}
	oss << " " << where.get();
	oss << " GROUP BY c.id ORDER BY COUNT(c.id) DESC";

	Wt::Dbo::Query<Cluster::pointer> query = session()->query<Cluster::pointer>( oss.str() );

	for (const std::string& bindArg : where.getBindArgs())
		query.bind(bindArg);

	Wt::Dbo::collection<Cluster::pointer> queryRes = query;

	std::map<IdType, std::vector<Cluster::pointer>> clusters;
	for (auto cluster : queryRes)
	{
		if (clusters[cluster->getType().id()].size() < size)
			clusters[cluster->getType().id()].push_back(cluster);
	}

	std::vector<std::vector<Cluster::pointer>> res;
	for (auto cluster_list : clusters)
		res.push_back(cluster_list.second);

	return res;
}

} // namespace Database
