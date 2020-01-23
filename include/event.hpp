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

namespace datapath {
	template<typename... _args>
	class event {
		std::list<std::function<void(_args...)>> _listeners;

		std::function<void()> _listen_cb;
		std::function<void()> _silence_cb;

		public /* functions */:

		// Destructor
		inline ~event()
		{
			this->clear();
		}

		// Add new listener.
		inline void add(std::function<void(_args...)> listener)
		{
			if (_listeners.size() == 0) {
				if (_listen_cb) {
					_listen_cb();
				}
			}
			_listeners.push_back(listener);
		}

		// Remove existing listener.
		inline void remove(std::function<void(_args...)> listener)
		{
			_listeners.remove(listener);
			if (_listeners.size() == 0) {
				if (_silence_cb) {
					silence_cb();
				}
			}
		}

		// Check if empty / no listeners.
		inline bool empty()
		{
			return _listeners.empty();
		}

		// Remove all listeners.
		inline void clear()
		{
			_listeners.clear();
			if (_silence_cb) {
				_silence_cb();
			}
		}

		public /* operators */:
		// Call Listeners with arguments.
		/// Not valid without the extra template.
		template<typename... _largs>
		inline void operator()(_args... args)
		{
			for (auto& l : _listeners) {
				l(args...);
			}
		}

		// Convert to bool (true if not empty, false if empty).
		inline operator bool()
		{
			return !this->empty();
		}

		// Add new listener.
		inline event<_args...>& operator+=(std::function<void(_args...)> listener)
		{
			this->add(listener);
			return *this;
		}

		// Remove existing listener.
		inline event<_args...>& operator-=(std::function<void(_args...)> listener)
		{
			this->remove(listener);
			return *this;
		}

		public /* events */:
		void set_listen_callback(std::function<void()> cb)
		{
			this->_listen_cb = cb;
		}

		void set_silence_callback(std::function<void()> cb)
		{
			this->_silence_cb = cb;
		}
	};
}; // namespace datapath
