#pragma once

#include "data_type.h"
#include "const_expr.h"
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ceres::casm
{
	// A data type as written, before constants are known. Each dimension is either an expression
	// (a literal, a constant, or arithmetic over them) or absent, meaning "work it out from the
	// initialiser". Any dimension may be absent, not just the outermost - a size is only an error
	// when the initialiser cannot supply it.
	class DataTypeReference
	{
	public:
		using Dimension = std::optional<ConstExpr>;

	private:
		DataTypeScalarCode _scalarCode = DataTypeScalarCode::Invalid;
		std::vector<Dimension> _dimensions;
		bool _isArray = false; // A scalar has no brackets at all; u8[] has one, empty

	public:
		DataTypeReference() noexcept = default;
		DataTypeReference(const DataTypeReference&) = default;
		DataTypeReference(DataTypeReference&&) noexcept = default;
		~DataTypeReference() = default;

		DataTypeReference& operator=(const DataTypeReference&) = default;
		DataTypeReference& operator=(DataTypeReference&&) noexcept = default;

		bool operator==(const DataTypeReference&) const = default;

	public:
		DataTypeReference(DataType dataType) noexcept :
			_scalarCode(dataType.scalarCode())
		{
			if (dataType.isScalar())
				return;

			_isArray = true;
			const u8 rank = dataType.rank();
			if (rank == 0)
			{
				// An unsized array (u8[], i.e. `string`): one dimension, left to the initialiser.
				_dimensions.emplace_back(std::nullopt);
				return;
			}
			for (u8 i = 0; i < rank; ++i)
				_dimensions.emplace_back(ConstExpr::makeLiteral(LiteralScalar::makeU32(dataType.dimension(i))));
		}

		DataTypeScalarCode scalarCode() const noexcept { return _scalarCode; }

		bool isValid() const noexcept { return _scalarCode != DataTypeScalarCode::Invalid; }
		bool isScalar() const noexcept { return isValid() && !_isArray; }
		bool isArray() const noexcept { return isValid() && _isArray; }
		u8 rank() const noexcept { return static_cast<u8>(_dimensions.size()); }

		std::span<const Dimension> dimensions() const noexcept { return _dimensions; }
		const Dimension& dimension(u8 index) const noexcept { return _dimensions[index]; }

		// True when at least one size is left for the initialiser to supply.
		bool hasInferredDimension() const noexcept
		{
			for (const auto& dimension : _dimensions)
			{
				if (!dimension.has_value())
					return true;
			}
			return false;
		}

		// True when nothing here can be settled without the symbol table: an inferred size, or one
		// written as anything other than a plain number.
		bool needsResolution() const noexcept
		{
			for (const auto& dimension : _dimensions)
			{
				if (!dimension.has_value() || !dimension->isLiteral())
					return true;
			}
			return false;
		}

		bool hasUnknownSize() const noexcept { return !isValid() || hasInferredDimension(); }

		// The flat element count, for the one-dimensional case the older checks still handle.
		std::optional<u32> literalElementCount() const noexcept
		{
			if (needsResolution())
				return std::nullopt;
			if (_dimensions.empty())
				return 1u;

			u32 total = 1;
			for (const auto& dimension : _dimensions)
				total *= dimension->literal().asRawValue();
			return total;
		}

		// Only possible when every dimension is a plain number - which is exactly when a check can
		// be made before the symbol table exists.
		std::optional<DataType> toLiteralDataType() const noexcept
		{
			if (needsResolution())
				return std::nullopt;
			if (_dimensions.empty())
				return DataType::makeScalar(_scalarCode);

			std::vector<u32> dimensions;
			dimensions.reserve(_dimensions.size());
			for (const auto& dimension : _dimensions)
				dimensions.push_back(dimension->literal().asRawValue());
			return DataType::makeArray(_scalarCode, dimensions);
		}

		std::string toString() const noexcept
		{
			std::string result{ DataType::scalarCodeToString(_scalarCode) };
			for (const auto& dimension : _dimensions)
				result += dimension.has_value() ? "[" + dimension->toString() + "]" : "[]";
			return result;
		}

	public:
		static DataTypeReference make(DataType dataType) noexcept { return DataTypeReference{ dataType }; }

		static DataTypeReference makeScalar(DataTypeScalarCode scalarCode) noexcept
		{
			DataTypeReference reference;
			reference._scalarCode = scalarCode;
			return reference;
		}

		static DataTypeReference makeArray(DataTypeScalarCode scalarCode, std::vector<Dimension>&& dimensions) noexcept
		{
			DataTypeReference reference;
			reference._scalarCode = scalarCode;
			reference._dimensions = std::move(dimensions);
			reference._isArray = true;
			return reference;
		}

	public:
		static const DataTypeReference Invalid;
	};

	inline const DataTypeReference DataTypeReference::Invalid = DataTypeReference{ DataType::Invalid };

}
