#pragma once

#include <string>
#include <string_view>
#include <array>
#include <concepts>

namespace ceres::string_utils
{
	template <typename T>
	concept StringLike = std::convertible_to<T, std::string_view>;

	template <StringLike... Args>
	inline constexpr std::string concat(Args&&... args)
	{
		if constexpr (sizeof...(args) == 0)
			return std::string();
		else
		{
			std::array<std::string_view, sizeof...(args)> views{ static_cast<std::string_view>(std::forward<Args>(args))... };

			std::size_t totalSize = 0;
			for (const auto& view : views)
				totalSize += view.size();

			std::string result;
			result.reserve(totalSize);
			for (const auto& view : views)
				result.append(view.data(), view.size());

			return result;
		}
	}

	template <StringLike... Args>
	inline constexpr std::string join(std::string_view delimiter, Args&&... args)
	{
		if constexpr (sizeof...(args) == 0)
			return std::string();
		else
		{
			std::array<std::string_view, sizeof...(args)> views{ static_cast<std::string_view>(std::forward<Args>(args))... };

			std::size_t totalSize = 0;
			for (const auto& view : views)
				totalSize += view.size();
			totalSize += delimiter.size() * (views.size() - 1);

			std::string result;
			result.reserve(totalSize);
			for (std::size_t i = 0; i < views.size(); ++i)
			{
				if (i > 0)
					result.append(delimiter.data(), delimiter.size());
				result.append(views[i].data(), views[i].size());
			}

			return result;
		}
	}
}
