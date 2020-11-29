# Automatically generated by pb2py
# fmt: off
import protobuf as p

from .CardanoAddressParametersType import CardanoAddressParametersType

if __debug__:
    try:
        from typing import Dict, List  # noqa: F401
        from typing_extensions import Literal  # noqa: F401
    except ImportError:
        pass


class CardanoTxOutputType(p.MessageType):

    def __init__(
        self,
        address: str = None,
        amount: int = None,
        address_parameters: CardanoAddressParametersType = None,
    ) -> None:
        self.address = address
        self.amount = amount
        self.address_parameters = address_parameters

    @classmethod
    def get_fields(cls) -> Dict:
        return {
            1: ('address', p.UnicodeType, 0),
            3: ('amount', p.UVarintType, 0),
            4: ('address_parameters', CardanoAddressParametersType, 0),
        }
