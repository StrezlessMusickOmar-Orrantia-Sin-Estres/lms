/*
 * Copyright (C) 2019 Emeric Poupon
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

#include "AuthTokenService.hpp"

#include <Wt/Auth/HashFunction.h>
#include <Wt/Auth/PasswordStrengthValidator.h>
#include <Wt/WRandom.h>

#include "auth/Types.hpp"
#include "database/Session.hpp"
#include "database/User.hpp"
#include "utils/Exception.hpp"
#include "utils/Logger.hpp"

namespace Auth
{

	std::unique_ptr<IAuthTokenService> createAuthTokenService(std::size_t maxThrottlerEntries)
	{
		return std::make_unique<AuthTokenService>(maxThrottlerEntries);
	}

	static const Wt::Auth::SHA1HashFunction sha1Function;

	AuthTokenService::AuthTokenService(std::size_t maxThrottlerEntries)
	: _loginThrottler {maxThrottlerEntries}
	{
	}

	std::string
	AuthTokenService::createAuthToken(Database::Session& session, Database::UserId userId, const Wt::WDateTime& expiry)
	{
		const std::string secret {Wt::WRandom::generateId(32)};
		const std::string secretHash {sha1Function.compute(secret, {})};

		auto transaction {session.createUniqueTransaction()};

		Database::User::pointer user {Database::User::getById(session, userId)};
		if (!user)
			throw Exception {"User deleted"};

		Database::AuthToken::pointer authToken {Database::AuthToken::create(session, secretHash, expiry, user)};

		LMS_LOG(UI, DEBUG) << "Created auth token for user '" << user->getLoginName() << "', expiry = " << expiry.toString();

		if (user->getAuthTokensCount() >= 50)
			Database::AuthToken::removeExpiredTokens(session, Wt::WDateTime::currentDateTime());

		return secret;
	}

	static
	std::optional<AuthTokenService::AuthTokenProcessResult::AuthTokenInfo>
	processAuthToken(Database::Session& session, std::string_view secret)
	{
		const std::string secretHash {sha1Function.compute(std::string {secret}, {})};

		auto transaction {session.createUniqueTransaction()};

		Database::AuthToken::pointer authToken {Database::AuthToken::getByValue(session, secretHash)};
		if (!authToken)
			return std::nullopt;

		if (authToken->getExpiry() < Wt::WDateTime::currentDateTime())
		{
			authToken.remove();
			return std::nullopt;
		}

		LMS_LOG(UI, DEBUG) << "Found auth token for user '" << authToken->getUser()->getLoginName() << "'!";

		AuthTokenService::AuthTokenProcessResult::AuthTokenInfo res {authToken->getUser()->getId(), authToken->getExpiry()};
		authToken.remove();

		return res;
	}

	AuthTokenService::AuthTokenProcessResult
	AuthTokenService::processAuthToken(Database::Session& session, const boost::asio::ip::address& clientAddress, std::string_view tokenValue)
	{
		// Do not waste too much resource on brute force attacks (optim)
		{
			std::shared_lock lock {_mutex};

			if (_loginThrottler.isClientThrottled(clientAddress))
				return AuthTokenProcessResult {AuthTokenProcessResult::State::Throttled};
		}

		auto res {Auth::processAuthToken(session, tokenValue)};
		{
			std::unique_lock lock {_mutex};

			if (_loginThrottler.isClientThrottled(clientAddress))
				return AuthTokenProcessResult {AuthTokenProcessResult::State::Throttled};

			if (!res)
			{
				_loginThrottler.onBadClientAttempt(clientAddress);
				return AuthTokenProcessResult {AuthTokenProcessResult::State::Denied};
			}

			_loginThrottler.onGoodClientAttempt(clientAddress);
			onUserAuthenticated(session, res->userId);
			return AuthTokenProcessResult {AuthTokenProcessResult::State::Granted, std::move(*res)};
		}
	}

	void
	AuthTokenService::clearAuthTokens(Database::Session& session, Database::UserId userId)
	{
		auto transaction {session.createUniqueTransaction()};

		Database::User::pointer user {Database::User::getById(session, userId)};
		if (!user)
			throw Exception {"User deleted"};

		user.modify()->clearAuthTokens();
	}

} // namespace Auth
