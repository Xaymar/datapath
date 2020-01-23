/*
 * Low Latency IPC Library for high-speed traffic
 * Copyright (C) 2017-2019 Michael Fabian Dirks <info@xaymar.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#pragma once
#include <functional>
#include <list>

#include "datapath.hpp"

namespace datapath {
	template<typename... _args>
	class event {
		std::list<std::function<void(_args...)>> _listeners;

		public:
		std::function<void(event<_args...>& ptr, std::function<void(_args...)>& fn)> on_add;
		std::function<void(event<_args...>& ptr, std::function<void(_args...)>& fn)> on_remove;

		public:
		event() : on_add(), on_remove()
		{
			_listeners.clear();
		};

		~event()
		{
			this->clear();
		}

		public /* Copy Constructor/Assignment */:
		event(const event<_args...>&) = delete;
		event<_args...>& operator=(const event<_args...>&) = delete;

		public /* Mode Constructor/Assignment */:
		event(event<_args...>&& rhs)
		{
			std::swap(_listeners, rhs._listeners);
		}
		event<_args...>& operator=(event<_args...>&& rhs)
		{
			std::swap(_listeners, rhs._listeners);
			return *this;
		};

		public /* Status */:
		// Check if empty / no listeners.
		inline bool empty()
		{
			return _listeners.empty();
		}

		// Convert to bool (true if not empty, false if empty).
		inline operator bool()
		{
			return !this->empty();
		}

		inline size_t count()
		{
			return _listeners.size();
		}

		public /* Listeners */:
		// Add new listener.
		inline void add(std::function<void(_args...)> listener)
		{
			if (on_add)
				on_add(*this, listener);
			_listeners.push_back(listener);
		}
		inline event<_args...>& operator+=(std::function<void(_args...)> listener)
		{
			this->add(listener);
			return *this;
		}

		// Remove existing listener.
		inline void remove(std::function<void(_args...)> listener)
		{
			_listeners.remove(listener);
			if (on_remove)
				on_remove(*this, listener);
		}
		inline event<_args...>& operator-=(std::function<void(_args...)> listener)
		{
			this->remove(listener);
			return *this;
		}

		// Remove all listeners.
		inline void clear()
		{
			_listeners.clear();
		}

		public /* Calling */:
		// Call Listeners with arguments.
		template<typename... _largs>
		inline void operator()(_args... args)
		{
			/// Not valid without the extra template.
			for (auto& l : _listeners) {
				l(args...);
			}
		}
	};
}; // namespace datapath
